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
 * R2: ordinary C. The slots are struct anim_slot in G.an; `start` and `pc`
 * hold guest addresses into G.ani, as the data area keeps them, and the
 * interpreter reads the instruction bytes through a pointer. No register
 * is part of any contract: the callers (the VM loop, the waits, the op 1a
 * handler) overwrite or ignore EAX, EBX, ECX, EDX and EDI after these,
 * and ESI is kept. Semantics: re-notes.md "Animation slots".
 */
#include <string.h>

#include "game/ctl.h"
#include "game/pace.h"
#include "game/state.h"
#include "game/vm.h"
#include "game/gmem.h"

#define ANIM_SLOTS AN_BASE
#define ANIM_SLOT_SIZE 24u
#define ANIM_SLOT_COUNT AN_SLOTS
#define ANIM_DATA 0x4a62beu       /* the ANI file */
#define ANIM_IDLE (AN_BASE + offsetof(struct anim_state, idle)) /* script start of a reset slot */

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

/* 0x40752c: copy the picture palette (16 + 240 u16 colours) into the
 * palette tables. */
void game_palette_copy(const uint16_t *src)
{
	memcpy(G.save.n.palette, src, 32);
	memcpy(G.rt.palette_picture, src + 16, 480);
}

void game_palette_copy_at(uint32_t src)
{
	game_palette_copy((const uint16_t *)g_ptr(src, 512));
}

/* slot n of the script: not range checked natively (a number past 9 runs on
 * into the words after the slots), so it is addressed, not indexed */
static struct anim_slot *slot_at(uint32_t n)
{
	return (struct anim_slot *)g_ptr(ANIM_SLOTS + n * ANIM_SLOT_SIZE, ANIM_SLOT_SIZE);
}

/* the blit of a rectangle record {x0, y0, w, h, dx, dy} (u16 each, the
 * surfaces in the cdecl's seventh argument as 0x100: surface 1 to 0) */
static void blit(int masked, uint32_t rect)
{
	uint16_t v[6];
	memcpy(v, g_ptr(rect, sizeof v), sizeof v);
	(masked ? game_blit_masked : game_blit_copy)(v[0], v[1], v[2], v[3], v[4], v[5], 1, 0);
}

/* 0x403661: run a slot for this frame. A stopped slot does nothing; a
 * delay counts down; otherwise instructions run until one that ends the
 * slot's frame (all but the blits and the palette). */
static void run_slot(struct anim_slot *s)
{
	for (;;) {
		uint32_t state = s->state;
		if (state == SLOT_STOPPED) {
			return;
		}
		if (s->delay) {
			s->delay--;
			return;
		}
		uint32_t pc = s->pc;
		const uint8_t *ip = g_ptr(pc, 3); /* the instruction: opcode, operand */
		uint32_t op = ip[0];
		pc++;
		switch (op) {
		case ANI_END:
			s->state = SLOT_STOPPED;
			break;
		case ANI_MARK:
			if (state == SLOT_TO_MARK) {
				s->state = SLOT_STOPPED;
			}
			break;
		case ANI_DELAY:
			s->delay = ip[1];
			pc++;
			break;
		case ANI_RESTART:
			pc = s->start;
			break;
		case ANI_LOOP_A:
		case ANI_LOOP_B: {
			uint32_t b = op == ANI_LOOP_B;
			s->count[b] = ip[1];
			pc++;
			s->loop[b] = pc;
			break;
		}
		case ANI_NEXT_A:
		case ANI_NEXT_B: {
			uint32_t b = op == ANI_NEXT_B;
			uint32_t n = (s->count[b] - 1u) & 0xff;
			s->count[b] = (uint8_t)n;
			if (n) {
				pc = s->loop[b];
			}
			break;
		}
		case ANI_COPY:
		case ANI_MASKED: {
			uint16_t off;
			memcpy(&off, ip + 1, 2);
			blit(op != ANI_COPY, ANIM_DATA + off);
			s->pc = pc + 2;
			continue;
		}
		case ANI_PALETTE: {
			uint16_t off;
			memcpy(&off, ip + 1, 2);
			game_palette_copy_at(ANIM_DATA + off);
			game_palette_upload();
			s->pc = pc + 2;
			continue;
		}
		default:
			game_trap(0x403684, "ANI instruction outside the native table");
		}
		s->pc = pc;
		return;
	}
}

/* 0x403624: once a frame, unless bit 1 of @[9] is set, run the ten slots
 * in order. */
void game_anim_step(void)
{
	if (SV(vflags) & 2) {
		return;
	}
	uint32_t frame = RT(frame);
	if (frame == AN(last_frame)) {
		return;
	}
	AN_SET(last_frame, frame);
	for (uint32_t i = 0; i < ANIM_SLOT_COUNT; i++) {
		run_slot(&G.an.slot[i]);
	}
}

/* 0x4035b5, the loop of sub-op 4: wait for a new frame, run the slot, and
 * again until its state is stopped. The native loop does not pump: it spins
 * on the frame counter, which the timer callback advances (each round of the
 * spin passes PACE_SPIN_US of game time, and runs the timer when it is due). */
static void wait_slot(struct anim_slot *s)
{
	for (;;) {
		uint32_t frame = RT(frame);
		if (frame == AN(last_frame)) {
			ctl_work_us(PACE_SPIN_US);
			continue;
		}
		AN_SET(last_frame, frame);
		run_slot(s);
		if (s->state)
			continue;
		return;
	}
}

/* the body of 0x403510 for the sub-ops that return at once (without the
 * `ret`) */
static void control(uint32_t op, uint32_t n, uint32_t a, uint32_t b)
{
	struct anim_slot *s = slot_at(n);
	switch (op) {
	case 0: {
		uint16_t off;
		memcpy(&off, g_ptr(2 * a + ANIM_DATA + 1, 2), 2);
		uint32_t start = ANIM_DATA + off;
		s->start = start;
		s->pc = start;
		s->state = 0;
		s->delay = 0;
		s->count[0] = s->count[1] = 0;
		s->x = 0;
		s->y = 0;
		break;
	}
	case 1:
		s->state = SLOT_RUNNING;
		s->pc = s->start;
		break;
	case 2:
		if (s->state != SLOT_STOPPED) {
			s->state = SLOT_TO_MARK;
		}
		break;
	case 3:
		s->state = SLOT_STOPPED;
		break;
	case 5:
		s->x = (uint16_t)a;
		s->y = (uint16_t)b;
		break;
	case 6:
		for (uint32_t i = 1; i <= 7; i++) {
			if (G.an.slot[i].state != SLOT_STOPPED) {
				G.an.slot[i].state = SLOT_TO_MARK;
			}
		}
		break;
	case 7:
		for (uint32_t i = 0; i < ANIM_SLOT_COUNT; i++) {
			G.an.slot[i].state = SLOT_STOPPED;
			G.an.slot[i].start = ANIM_IDLE;
		}
		break;
	default:
		game_trap(0x40351a, "animation sub-op outside the native table");
	}
}

/* 0x403510: slot control, called by op 1a with the sub-op, the slot number
 * and the two operands:
 *   0 a = n: load script n of the ANI file, stopped and cleared;
 *   1 start it from its first instruction;
 *   2 stop it at its next mark (if running);
 *   3 stop it now;
 *   4 start it and wait until it stops;
 *   5 a, b = x, y (stored, unused);
 *   6 slots 1-7: stop each at its next mark (if running);
 *   7 all slots: stop and point at ANIM_IDLE. */
void game_anim_control(uint32_t op, uint32_t n, uint32_t a, uint32_t b)
{
	if (op == 4) {
		struct anim_slot *s = slot_at(n);
		s->state = SLOT_RUNNING;
		s->pc = s->start;
		wait_slot(s);
		return;
	}
	control(op, n, a, b);
}

/* 0x406dff: reset all slots (sub-op 7, slot 0). */
void game_anim_reset(void)
{
	control(7, 0, 0, 0);
}
