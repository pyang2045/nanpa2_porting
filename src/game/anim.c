/*
 * Animation slots (stage 4): the slot control 0x403510, the per-frame step
 * 0x403624, the ANI bytecode interpreter 0x403661, the slot reset 0x406dff
 * and the picture palette copy 0x40752c it uses.
 *
 * Ten slots of 24 bytes at ANIM_SLOTS run scripts from the ANI file that
 * op 04 1c loads at ANIM_DATA (its byte 1 on: a u16 offset per script,
 * then the scripts, rectangles and palettes, all as offsets from
 * ANIM_DATA). Once per frame (frame counter 0x446292, at most one step a
 * frame: ANIM_LAST_FRAME) every running slot runs its script until a
 * delay, a mark or its end; blits and palette changes in between go on in
 * the same frame. The blits (0x401430, 0x4014f0) and the palette upload
 * (0x401ca0) are the native screen routines, called as the native code
 * calls them.
 *
 * Register effects are the native ones (the callers' callers read EAX,
 * EBX, ECX, EDX or EDI in places tools/liveness.py cannot rule out), so
 * each unit's only scratch is the flags. Semantics: re-notes.md
 * "Animation slots".
 */
#include "game/vm.h"
#include "x86/x86.h"

#define ANIM_SLOTS 0x4312ccu
#define ANIM_SLOT_SIZE 24u
#define ANIM_SLOT_COUNT 10u
#define ANIM_DATA 0x4a62beu       /* the ANI file */
#define ANIM_LAST_FRAME 0x4313bcu /* u32: the frame the slots last ran */
#define ANIM_IDLE 0x4313c0u       /* script start of a reset slot */
#define FRAME 0x446292u           /* u32: frames since start (60 Hz timer) */
#define VM_FLAGS9 0x4452e0u       /* u8 @[9]: bit 1 stops the per-frame step */
#define PALETTE_BASE 0x44530cu    /* u16[16]: the base colours */
#define PALETTE_REST 0x445eaeu    /* u16[240]: the picture's colours */

#define BLIT_COPY 0x401430u   /* cdecl (x0, y0, w, h, dx, dy, surfaces) */
#define BLIT_MASKED 0x4014f0u /* the same, colour 8 transparent */
#define PALETTE_SET 0x401ca0u /* palette tables to the DirectDraw palette */

/* one slot */
enum {
	SLOT_STATE = 0,   /* u8: SLOT_STOPPED, SLOT_TO_MARK or SLOT_RUNNING */
	SLOT_DELAY = 1,   /* u8: frames to wait before the next instruction */
	SLOT_COUNT_A = 2, /* u8: loop counters */
	SLOT_COUNT_B = 3,
	SLOT_START = 4,   /* u32: the script's first instruction */
	SLOT_PC = 8,      /* u32: the next instruction */
	SLOT_LOOP_A = 12, /* u32: loop starts */
	SLOT_LOOP_B = 16,
	SLOT_X = 20,      /* u16 x, u16 y: set by sub-op 5, never read */
	SLOT_Y = 22,
};
enum { SLOT_STOPPED = 0x00, SLOT_TO_MARK = 0x01, SLOT_RUNNING = 0xff };

/* ANI instructions */
enum {
	ANI_END = 0x00,    /* stop */
	ANI_MARK = 0x01,   /* stop here if asked to (sub-ops 2 and 6) */
	ANI_DELAY = 0x02,  /* n: wait n frames */
	ANI_RESTART = 0x03,
	ANI_LOOP_A = 0x04, /* n: loop A n times from here */
	ANI_NEXT_A = 0x05,
	ANI_LOOP_B = 0x06,
	ANI_NEXT_B = 0x07,
	ANI_COPY = 0x08,   /* off: copy the rectangle at ANIM_DATA + off */
	ANI_MASKED = 0x09, /* off: the same, masked */
	ANI_PALETTE = 0x0a /* off: the palette at ANIM_DATA + off */
};

/* 0x40752c: copy the picture palette at src (16 + 240 u16 colours) into
 * the palette tables. Registers and flags are kept. */
void game_palette_copy(uint32_t src)
{
	for (uint32_t i = 0; i < 8; i++)
		wr32(PALETTE_BASE + 4 * i, rd32(src + 4 * i));
	for (uint32_t i = 0; i < 0x78; i++)
		wr32(PALETTE_REST + 4 * i, rd32(src + 32 + 4 * i));
	X86_COUNT(14 + 8 + 0x78);
}

void game_palette_copy_regs(void)
{
	game_palette_copy(cpu.r[EAX]);
}

/* the blit of a rectangle record {x0, y0, w, h, dx, dy} (u16 each, the
 * surfaces in the cdecl's seventh argument as 0x100: surface 1 to 0) */
static void blit(uint32_t fn, uint32_t rect)
{
	cpu.r[EDI] = rect;
	push32(0x100);
	for (int i = 5; i >= 0; i--) {
		set16(EAX, rd16(rect + 2u * (uint32_t)i));
		push32(cpu.r[EAX]);
	}
	X86_COUNT(17);
	game_ncall(fn, 0);
	cpu.r[ESP] += 0x1c;
	X86_COUNT(1);
}

/* 0x403661: run slot s (ESI = its offset) for this frame. A stopped slot
 * does nothing; a delay counts down; otherwise instructions run until one
 * that ends the slot's frame (all but the blits and the palette). Out:
 * EBX = the slot's next instruction when one ran; EAX, EDI, ECX, EDX as
 * the native code leaves them. */
void game_anim_run_slot(void)
{
	uint32_t slot = ANIM_SLOTS + cpu.r[ESI];
	for (;;) {
		X86_COUNT(2);
		uint32_t state = rd8(slot + SLOT_STATE);
		if (state == SLOT_STOPPED) {
			X86_COUNT(1);
			return;
		}
		X86_COUNT(2);
		uint32_t delay = rd8(slot + SLOT_DELAY);
		if (delay) {
			wr8(slot + SLOT_DELAY, delay - 1);
			X86_COUNT(2);
			return;
		}
		uint32_t pc = rd32(slot + SLOT_PC);
		uint32_t op = rd8(pc++);
		cpu.r[EAX] = op;
		cpu.r[EBX] = pc;
		X86_COUNT(4);
		switch (op) {
		case ANI_END:
			wr8(slot + SLOT_STATE, SLOT_STOPPED);
			X86_COUNT(2);
			break;
		case ANI_MARK:
			X86_COUNT(2);
			if (state == SLOT_TO_MARK) {
				wr8(slot + SLOT_STATE, SLOT_STOPPED);
				X86_COUNT(2);
			}
			break;
		case ANI_DELAY:
			cpu.r[EAX] = rd8(pc++);
			wr8(slot + SLOT_DELAY, cpu.r[EAX]);
			X86_COUNT(4);
			break;
		case ANI_RESTART:
			pc = rd32(slot + SLOT_START);
			X86_COUNT(2);
			break;
		case ANI_LOOP_A:
		case ANI_LOOP_B: {
			uint32_t b = op == ANI_LOOP_B;
			cpu.r[EAX] = rd8(pc++);
			wr8(slot + SLOT_COUNT_A + b, cpu.r[EAX]);
			wr32(slot + SLOT_LOOP_A + 4 * b, pc);
			X86_COUNT(5);
			break;
		}
		case ANI_NEXT_A:
		case ANI_NEXT_B: {
			uint32_t b = op == ANI_NEXT_B;
			uint32_t n = (rd8(slot + SLOT_COUNT_A + b) - 1) & 0xff;
			wr8(slot + SLOT_COUNT_A + b, n);
			X86_COUNT(2);
			if (n) {
				pc = rd32(slot + SLOT_LOOP_A + 4 * b);
				X86_COUNT(2);
			}
			break;
		}
		case ANI_COPY:
		case ANI_MASKED: {
			uint32_t rect = ANIM_DATA + rd16(pc);
			cpu.r[EBX] = pc + 2;
			blit(op == ANI_COPY ? BLIT_COPY : BLIT_MASKED, rect);
			wr32(slot + SLOT_PC, cpu.r[EBX]);
			X86_COUNT(2);
			X86_POLL(op == ANI_COPY ? 0x403787 : 0x4037c8);
			continue;
		}
		case ANI_PALETTE:
			cpu.r[EAX] = ANIM_DATA + rd16(pc);
			cpu.r[EBX] = pc + 2;
			X86_COUNT(6);
			game_palette_copy(cpu.r[EAX]);
			X86_COUNT(2);
			game_ncall(PALETTE_SET, 0);
			wr32(slot + SLOT_PC, cpu.r[EBX]);
			X86_COUNT(2);
			X86_POLL(0x4037ed);
			continue;
		default:
			x86_trap(0x403684, "ANI instruction outside the native table");
		}
		cpu.r[EBX] = pc;
		wr32(slot + SLOT_PC, pc);
		X86_COUNT(2);
		return;
	}
}

/* 0x403624: once a frame, unless bit 1 of @[9] is set, run the ten slots
 * in order. Out: EAX = the frame (unless @[9] bit 1), ECX = 0 and EBX,
 * EDI, EDX as the last slot left them when the slots ran; ESI kept. */
void game_anim_step(void)
{
	X86_COUNT(2);
	if (rd8(VM_FLAGS9) & 2) {
		X86_COUNT(1);
		return;
	}
	uint32_t frame = rd32(FRAME);
	cpu.r[EAX] = frame;
	X86_COUNT(3);
	if (frame == rd32(ANIM_LAST_FRAME)) {
		X86_COUNT(1);
		return;
	}
	wr32(ANIM_LAST_FRAME, frame);
	push32(cpu.r[ESI]);
	X86_COUNT(3);
	for (uint32_t n = ANIM_SLOT_COUNT; n; n--) {
		cpu.r[ECX] = n;
		push32(n);
		uint32_t i = ANIM_SLOT_COUNT - n;
		cpu.r[EBX] = i << 4;
		cpu.r[ESI] = i * ANIM_SLOT_SIZE;
		X86_COUNT(8);
		game_anim_run_slot();
		cpu.r[ECX] = pop32() - 1;
		X86_COUNT(2);
		if (n > 1)
			X86_POLL(0x40365d);
	}
	cpu.r[ESI] = pop32();
	X86_COUNT(2);
}

/* 0x403510: slot control, EAX = sub-op, EBX = slot (EDI = its offset,
 * EBX = slot << 4 on return):
 *   0 ECX = n: load script n of the ANI file, stopped and cleared;
 *   1 start it from its first instruction;
 *   2 stop it at its next mark (if running);
 *   3 stop it now;
 *   4 start it and wait until it stops (the native code, which spins on
 *     the frame counter: game_native_anim_control);
 *   5 ECX, EDX = x, y (stored, unused);
 *   6 slots 1-7: stop each at its next mark (if running);
 *   7 all slots: stop and point at ANIM_IDLE (EAX = ANIM_IDLE). */
int game_native_anim_control(void)
{
	return cpu.r[EAX] == 4;
}

void game_anim_control_regs(void)
{
	uint32_t op = cpu.r[EAX], n = cpu.r[EBX];
	uint32_t s = n * ANIM_SLOT_SIZE, slot = ANIM_SLOTS + s;
	cpu.r[EDI] = s;
	cpu.r[EBX] = n << 4;
	X86_COUNT(5);
	switch (op) {
	case 0: {
		uint32_t entry = 2 * cpu.r[ECX] + ANIM_DATA + 1;
		uint32_t start = ANIM_DATA + rd16(entry);
		cpu.r[ECX] = entry;
		cpu.r[EAX] = start;
		wr32(slot + SLOT_START, start);
		wr32(slot + SLOT_PC, start);
		wr32(slot + SLOT_STATE, 0); /* state, delay, both loop counts */
		wr32(slot + SLOT_X, 0);
		X86_COUNT(10);
		break;
	}
	case 1:
		wr8(slot + SLOT_STATE, SLOT_RUNNING);
		cpu.r[EAX] = rd32(slot + SLOT_START);
		wr32(slot + SLOT_PC, cpu.r[EAX]);
		X86_COUNT(4);
		break;
	case 2:
		X86_COUNT(3);
		if (rd8(slot + SLOT_STATE) != SLOT_STOPPED) {
			wr8(slot + SLOT_STATE, SLOT_TO_MARK);
			X86_COUNT(1);
		}
		break;
	case 3:
		wr8(slot + SLOT_STATE, SLOT_STOPPED);
		X86_COUNT(2);
		break;
	case 5:
		wr16(slot + SLOT_X, cpu.r[ECX]);
		wr16(slot + SLOT_Y, cpu.r[EDX]);
		X86_COUNT(3);
		break;
	case 6:
		X86_COUNT(2);
		for (uint32_t i = 1; i <= 7; i++) {
			uint32_t t = ANIM_SLOTS + i * ANIM_SLOT_SIZE;
			X86_COUNT(4);
			if (rd8(t + SLOT_STATE) != SLOT_STOPPED) {
				wr8(t + SLOT_STATE, SLOT_TO_MARK);
				X86_COUNT(1);
			}
			if (i < 7)
				X86_POLL(0x403602);
		}
		cpu.r[EDI] = 8 * ANIM_SLOT_SIZE;
		cpu.r[ECX] = 0;
		X86_COUNT(1);
		break;
	case 7:
		X86_COUNT(2);
		for (uint32_t i = 0; i < ANIM_SLOT_COUNT; i++) {
			uint32_t t = ANIM_SLOTS + i * ANIM_SLOT_SIZE;
			wr8(t + SLOT_STATE, SLOT_STOPPED);
			wr32(t + SLOT_START, ANIM_IDLE);
			X86_COUNT(5);
			if (i < ANIM_SLOT_COUNT - 1)
				X86_POLL(0x403621);
		}
		cpu.r[EAX] = ANIM_IDLE;
		cpu.r[EDI] = ANIM_SLOT_COUNT * ANIM_SLOT_SIZE;
		cpu.r[ECX] = 0;
		X86_COUNT(1);
		break;
	default:
		x86_trap(0x40351a, "animation sub-op outside the native table");
	}
}

/* 0x406dff: reset all slots (sub-op 7, slot 0); ESI kept. */
void game_anim_reset(void)
{
	cpu.r[EAX] = 7;
	cpu.r[EBX] = 0;
	X86_COUNT(4);
	game_anim_control_regs();
	X86_COUNT(2);
}
