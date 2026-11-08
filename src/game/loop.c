/*
 * The script VM's own loop (stage 4): the entry 0x4060d4, the loop head
 * 0x4060f9 with the click handlers, the opcode fetch and dispatch
 * (0x40616e), blocks (01 ... 00 / 02), text (06 ... 06 and Big5
 * characters), and the waits every pass and the handlers use: the input
 * test 0x4074d5, the frame wait 0x4075c6 and the click wait 0x406acc.
 *
 * Every pass of the loop pumps messages (0x401bf0), steps the animations
 * (0x403624), tests the left and right button for the click handlers that
 * ext op 20 sets (0x4074d5 twice, which pumps again), then fetches one
 * opcode. Like the handlers in src/game/ops_vm.c, the loop is split at the
 * calls that pump messages, wait or run a block: the C piece pushes the
 * return address and goes on at the callee (call_exit), and the code after
 * the call is a piece of its own (src/hle/overrides.h, HLE_GAME_CALL_EXIT
 * and the HLE_GAME_JUMP lines of this file). So the pump runs once,
 * natively, between pieces, a nested block is the same loop on a deeper
 * stack, and a piece's oracle ends where the C piece does.
 *
 * A block (01) and the VM itself end at 00 or 02: at the top level the VM
 * ends (0x40638d unwinds to the entry's frame and returns to WinMain),
 * inside a block the loop returns to whoever ran the block with AL = 0.
 * Bytes 03, 05, 07, 08 and 0x20-0x7f end the VM too.
 *
 * Pieces read and write the CPU as the native code there does. The click
 * handler path (a button down with its handler set: wait for the release,
 * then run the handler's block) is left to the native code
 * (game_native_click*), as ext op 20 that sets the handlers is: no run has
 * reached it.
 */
#include "game/file.h"
#include "game/vm.h"
#include "x86/x86.h"

#define PUMP 0x401bf0u        /* message pump: EAX = 0 on WM_QUIT */
#define LOAD_FILE 0x401050u   /* cdecl (name, buffer) */
#define VM_END 0x40638du      /* mov esp, [VM_TOP_ESP]; pop ebp; ret */
#define VM_UNWOUND 0x406393u  /* after that mov: where an oracle stops */
#define VM_TOP_ESP 0x445cdcu  /* ESP of the top-level VM (after its push ebp) */
#define CLICK_BUSY 0x445ce1u  /* u8: a click handler's block is running */
#define INPUTS 0x445d8fu      /* u8: buttons and keys down (re-notes.md "Input") */
#define INPUT_SKIP 0x40u      /* Ctrl: no waiting for clicks */
#define VM_FLAGS9 0x4452e0u   /* u8 @[9]: bit 2 turns the click handlers off */
#define TEXT_DELAY 0x4452deu  /* u16 @[8]: frames (x4) after each character */
#define TEXT_SIZE 0x4452f6u   /* u8: Big5 font size */
#define TEXT_X 0x4452f0u
#define TEXT_Y 0x4452f2u
#define FRAME 0x446292u
#define BIG5_NEWLINE 0xa1beu

#define RUN_BLOCK 0x4061cdu
#define INPUT_TEST 0x4074d5u
#define WAIT_FRAMES 0x4075c6u
#define WAIT_CLICK 0x406accu

/* the native `call target` with return address ra */
static uint32_t call_exit(uint32_t target, uint32_t ra)
{
	push32(ra);
	X86_COUNT(1);
	return target;
}

/* WM_QUIT (the pump returned 0) in a piece with words on the stack:
 * 0x40638d's `mov esp, [VM_TOP_ESP]`, which takes ESP above the piece's
 * own level, where its oracle stops; the rest (pop ebp, ret to WinMain)
 * runs natively */
static uint32_t quit(void)
{
	cpu.r[ESP] = rd32(VM_TOP_ESP);
	X86_COUNT(1);
	return VM_UNWOUND;
}

/* ---- the entry and the loop head */

/* 0x4060d4, called by WinMain: save EBP and the top-level ESP, set up the
 * VM state (0x406395), load the MES file named at VM_SCRIPT_NAME (START.MES
 * at first) and start at its entry (its first word). */
uint32_t game_vm_entry(void)
{
	push32(cpu.r[EBP]);
	wr32(VM_TOP_ESP, cpu.r[ESP]);
	X86_COUNT(3);
	game_vm_init_regs();
	push32(VM_SCRIPT);
	push32(VM_SCRIPT_NAME);
	X86_COUNT(3);
	game_file_load(rd32(cpu.r[ESP]), rd32(cpu.r[ESP] + 4));
	cpu.r[ESP] += 8;
	cpu.r[ESI] = rd16(VM_SCRIPT);
	X86_COUNT(2);
	return VM_LOOP;
}

/* 0x4060f9: each pass starts with the message pump */
uint32_t game_vm_loop(void)
{
	return call_exit(PUMP, 0x4060fe);
}

static uint32_t fetch(void);

/* 0x4060fe, after the pump: WM_QUIT ends the VM; else step the
 * animations, then test the left button for its handler, unless the
 * handlers are off (@[9] bit 2) or one is running. */
uint32_t game_vm_pumped(void)
{
	X86_COUNT(2);
	if (!cpu.r[EAX])
		return VM_END; /* at this piece's ESP, where its oracle stops too */
	X86_COUNT(1);
	game_anim_step();
	X86_COUNT(2);
	if (rd8(VM_FLAGS9) & 4)
		return fetch();
	X86_COUNT(2);
	if (rd8(CLICK_BUSY))
		return fetch();
	set8l(EAX, 0);
	X86_COUNT(1);
	return call_exit(INPUT_TEST, 0x406124);
}

/* the click handler path: button k down (ZF clear) with handler k set */
int game_native_click0(void)
{
	return !cpu.zf && rd16(VM_CLICK_HANDLERS) != 0;
}

int game_native_click1(void)
{
	return !cpu.zf && rd16(VM_CLICK_HANDLERS + 2) != 0;
}

/* 0x406124, after testing the left button (ZF set: up): down, with no
 * handler, goes straight to the fetch, skipping the right button; up
 * tests the right one. */
uint32_t game_vm_click_tested0(void)
{
	cpu.r[EDI] = rd16(VM_CLICK_HANDLERS);
	X86_COUNT(2);
	if (!cpu.zf) {
		if (cpu.r[EDI])
			x86_trap(0x40613f, "click handler path reached C (game_native_click0)");
		f_logic(cpu.r[EDI], 32); /* or edi, edi: 0 here */
		X86_COUNT(2);
		return fetch();
	}
	set8l(EAX, 1);
	X86_COUNT(1);
	return call_exit(INPUT_TEST, 0x406134);
}

/* 0x406134, after testing the right button */
uint32_t game_vm_click_tested1(void)
{
	cpu.r[EDI] = rd16(VM_CLICK_HANDLERS + 2);
	X86_COUNT(2);
	if (!cpu.zf) {
		if (cpu.r[EDI])
			x86_trap(0x40613f, "click handler path reached C (game_native_click1)");
		f_logic(cpu.r[EDI], 32);
		X86_COUNT(2);
		return fetch();
	}
	X86_COUNT(1);
	return fetch();
}

/* ---- fetch and dispatch */

/* handlers of ops 0a-1a (table 0x40620b; 10 is a no-op, the loop head) and
 * of the extended ops 04 10-04 26 (table 0x406262), as offsets from
 * HANDLER_BASE (16 bits, so the binary does not hold the native tables:
 * ctest no_game_bytes) */
#define HANDLER_BASE 0x400000u
static const uint16_t op_handlers[] = {
	0x6491, 0x64b5, 0x64d9, 0x64f8, 0x6557, 0x659e, 0x60f9, 0x65f6, 0x663e,
	0x6658, 0x6a9a, 0x69ce, 0x6aad, 0x6b05, 0x69be, 0x7204, 0x7096,
};
static const uint16_t ext_handlers[] = {
	0x660c, 0x6638, 0x663b, 0x6671, 0x6842, 0x6916, 0x6f06, 0x6b76, 0x6bcb,
	0x6c20, 0x6c85, 0x6cea, 0x6d8b, 0x6e11, 0x6f7c, 0x6fcd, 0x70bc, 0x70d5,
	0x739b, 0x71a4, 0x73a5, 0x73cd, 0x73ee,
};
#define N_OPS (sizeof op_handlers / sizeof op_handlers[0])
#define N_EXT (sizeof ext_handlers / sizeof ext_handlers[0])

/* 00 or 02 (0x4061aa): the end of the running block, or of the VM at the
 * top level; a block returns with AL = 0 (EAX's upper bytes are ESP's,
 * from the native test) */
static uint32_t block_end(void)
{
	cpu.r[ESI]++;
	cpu.r[EAX] = cpu.r[ESP];
	X86_COUNT(4);
	if (cpu.r[ESP] == rd32(VM_TOP_ESP))
		return VM_END;
	set8l(EAX, 0);
	X86_COUNT(2);
	return pop32();
}

/* 01 (0x4061bc): run the block (0x4061cd: past the 01, a nested loop) with
 * ESI saved; game_vm_block_done goes on after it */
static uint32_t block_open(void)
{
	push32(cpu.r[ESI]);
	X86_COUNT(1);
	return call_exit(RUN_BLOCK, 0x4061c2);
}

/* the character delay after a character, then the piece at ra (the cursor
 * advance; a jump there from the native code ends a piece too) */
static uint32_t char_delay(uint32_t ra)
{
	cpu.r[ECX] = rd16(TEXT_DELAY);
	X86_COUNT(3);
	if (!cpu.r[ECX])
		return ra;
	return call_exit(WAIT_FRAMES, ra);
}

/* the text colour as the native code pushes it: bg = colour >> 4 (all of
 * it), fg = its low nibble */
static uint32_t colour_bg(void)
{
	return rd16(VM_TEXT_COLOUR) >> 4;
}

static uint32_t colour_fg(void)
{
	return rd16(VM_TEXT_COLOUR) & 0xf;
}

/* 0x4062bf: the next byte of a 06 string: 06 ends it, any other is one
 * ASCII character (size 0) at the cursor */
static uint32_t string_char(void)
{
	uint32_t c = vm_script_byte(cpu.r[ESI]++);
	set8l(EAX, c);
	X86_COUNT(4);
	if (c == VM_STRING) {
		X86_POLL(0x4062c8);
		return VM_LOOP;
	}
	uint32_t bg = colour_bg(), fg = colour_fg(), y = rd16(TEXT_Y), x = rd16(TEXT_X) << 3;
	cpu.r[EDX] = x;
	cpu.r[EAX] = c;
	X86_COUNT(15);
	x86_probe(0x4062ff);
	game_draw_char(c, x, y, 0, fg, bg);
	return char_delay(0x406315);
}

/* a byte from 0x80 (0x40631c) and the next: one Big5 character, A1BE a
 * newline */
static uint32_t big5_char(void)
{
	uint32_t pc = cpu.r[ESI] + 1;
	uint32_t c = (cpu.r[EAX] & 0xff) << 8 | vm_script_byte(pc++);
	cpu.r[ESI] = pc;
	set16(EAX, c);
	X86_COUNT(6);
	if (c == BIG5_NEWLINE) {
		X86_COUNT(1);
		game_text_newline();
		X86_COUNT(1);
		X86_POLL(0x406331);
		return VM_LOOP;
	}
	uint32_t bg = colour_bg(), fg = colour_fg(), size = rd8(TEXT_SIZE), y = rd16(TEXT_Y),
	         x = rd16(TEXT_X) << 3;
	cpu.r[EDX] = x;
	cpu.r[EAX] = c;
	X86_COUNT(16);
	x86_probe(0x40636d);
	game_draw_char(c, x, y, size, fg, bg);
	return char_delay(0x406383);
}

/* 0x40616e: fire the fetch probe (the exploration bot and NANPA2_VMTRACE
 * watch it), read the opcode at ESI and go on at what it is */
static uint32_t fetch(void)
{
	x86_probe(0x40616e);
	uint32_t op = vm_script_byte(cpu.r[ESI]);
	set8l(EAX, op);
	X86_COUNT(1);
	if (op == VM_BLOCK_END) {
		X86_COUNT(2);
		return block_end();
	}
	if (op == VM_BLOCK_OPEN) {
		X86_COUNT(4);
		return block_open();
	}
	if (op == 0x04) { /* 04 xx: extended op (no bound check natively) */
		uint32_t xx = vm_script_byte(cpu.r[ESI] + 1);
		cpu.r[ESI] += 2;
		cpu.r[EAX] = xx - 0x10;
		X86_COUNT(6 + 5);
		if (cpu.r[EAX] >= N_EXT)
			x86_trap(0x40625b, "extended opcode outside the native table");
		return HANDLER_BASE + ext_handlers[cpu.r[EAX]];
	}
	if (op == VM_SEPARATOR) {
		X86_COUNT(8);
		return block_end();
	}
	if (op == VM_STRING) {
		cpu.r[ESI]++;
		X86_COUNT(10 + 1);
		return string_char();
	}
	if (op >= 0x80) {
		X86_COUNT(12);
		return big5_char();
	}
	if (op < 0x09 || op >= 0x20) {
		X86_COUNT(op < 0x09 ? 14 : 16);
		return VM_END;
	}
	cpu.r[ESI]++;
	cpu.r[EAX] = op - 0x0a;
	X86_COUNT(17 + 4);
	if (cpu.r[EAX] >= N_OPS)
		x86_trap(0x406204, "opcode outside the native table (09, 1b-1f)");
	return HANDLER_BASE + op_handlers[cpu.r[EAX]];
}

/* ---- blocks */

/* 0x4061c2, after a block: ESI back at its 01, skip it (0x4061d4, AX = 0)
 * and go on */
uint32_t game_vm_block_done(void)
{
	cpu.r[ESI] = vm_skip_block(pop32());
	set16(EAX, 0);
	X86_COUNT(3);
	X86_POLL(0x4061c8);
	return VM_LOOP;
}

/* 0x4061cd, called with ESI at a block's 01: run the block as a nested
 * loop (`call 0x4060f9`); 0x4061d3 returns its AL */
uint32_t game_vm_run_block(void)
{
	cpu.r[ESI]++;
	X86_COUNT(1);
	return call_exit(VM_LOOP, 0x4061d3);
}

uint32_t game_vm_block_return(void)
{
	X86_COUNT(1);
	return pop32();
}

/* ---- text */

/* 0x406315, after an ASCII character (and its delay): advance the cursor,
 * then the next byte of the string */
uint32_t game_vm_string_next(void)
{
	X86_COUNT(1);
	game_text_advance();
	X86_COUNT(1);
	X86_POLL(0x40631a);
	return string_char();
}

/* 0x406383, after a Big5 character: advance the cursor, go on */
uint32_t game_vm_big5_next(void)
{
	X86_COUNT(1);
	game_text_advance();
	X86_COUNT(1);
	X86_POLL(0x406388);
	return VM_LOOP;
}

/* ---- waits */

/* 0x4074d5, AL = k: pump, then test input k: 0 click/Enter (0x10), 1
 * right click/Esc (0x20), 2-5 up, down, left, right (1, 2, 4, 8); ZF set
 * when it is up. WM_QUIT ends the VM. */
uint32_t game_input_test(void)
{
	push32(cpu.r[EAX]);
	X86_COUNT(1);
	return call_exit(PUMP, 0x4074db);
}

uint32_t game_input_tested(void)
{
	static const uint8_t bit[6] = { 0x10, 0x20, 0x01, 0x02, 0x04, 0x08 };
	X86_COUNT(2);
	if (!f_logic(cpu.r[EAX], 32)) /* or eax, eax: the flags a quit leaves */
		return quit();
	cpu.r[EAX] = pop32();
	uint32_t k = cpu.r[EAX] & 0xff;
	X86_COUNT(1 + 2 * (k < 6 ? k + 1 : 6));
	if (k < 6) {
		f_logic(rd8(INPUTS) & bit[k], 8);
		X86_COUNT(2);
	} else {
		f_sub(k, 5, 0, 8); /* the last `cmp al, 5` */
		X86_COUNT(1);
	}
	return pop32();
}

/* 0x4075c6, ECX = n: wait until the frame counter reaches its value now +
 * n / 4 + 1, pumping and stepping the animations each round (ECX =
 * n / 4 + 1, EAX = the target frame) */
uint32_t game_wait_frames(void)
{
	cpu.r[ECX] = (cpu.r[ECX] >> 2) + 1;
	cpu.r[EAX] = rd32(FRAME) + cpu.r[ECX];
	push32(cpu.r[EAX]);
	X86_COUNT(5);
	return call_exit(PUMP, 0x4075d7);
}

/* 0x4075d7, after a pump, the target frame on the stack */
uint32_t game_wait_frames_pumped(void)
{
	X86_COUNT(2);
	if (!cpu.r[EAX])
		return quit();
	X86_COUNT(1);
	game_anim_step();
	cpu.r[EAX] = pop32();
	f_sub(cpu.r[EAX], rd32(FRAME), 0, 32);
	X86_COUNT(3);
	if (!cpu.cf && !cpu.zf) { /* ja: not there yet */
		X86_POLL(0x4075eb);
		push32(cpu.r[EAX]);
		X86_COUNT(1);
		return call_exit(PUMP, 0x4075d7);
	}
	X86_COUNT(1);
	return pop32();
}

/* 0x406acc / 0x406ae8: with Ctrl down return at once (AL = 0x40), else
 * wait a frame and test the button (the piece at ra) */
static uint32_t click_wait_round(uint32_t ra)
{
	set8l(EAX, f_logic(rd8(INPUTS) & INPUT_SKIP, 8));
	X86_COUNT(3);
	if (cpu.r[EAX] & 0xff) {
		X86_COUNT(1);
		return pop32();
	}
	cpu.r[ECX] = 1;
	X86_COUNT(1);
	return call_exit(WAIT_FRAMES, ra);
}

/* 0x406acc: wait for a click or Enter, pressed (one frame per test) and
 * then released */
uint32_t game_wait_click(void)
{
	return click_wait_round(0x406adf);
}

/* 0x406adf / 0x406afb, after a frame: test the button */
uint32_t game_wait_click_framed(void)
{
	set8l(EAX, 0);
	X86_COUNT(1);
	return call_exit(INPUT_TEST, 0x406ae6);
}

uint32_t game_wait_click_release_framed(void)
{
	set8l(EAX, 0);
	X86_COUNT(1);
	return call_exit(INPUT_TEST, 0x406b02);
}

/* 0x406ae6, tested (ZF set: up): up waits on for the press, down waits
 * for the release */
uint32_t game_wait_click_tested(void)
{
	X86_COUNT(1);
	if (cpu.zf) {
		X86_POLL(0x406ae6);
		return WAIT_CLICK;
	}
	return click_wait_round(0x406afb);
}

/* 0x406b02, tested while waiting for the release */
uint32_t game_wait_click_release_tested(void)
{
	X86_COUNT(1);
	if (!cpu.zf) {
		X86_POLL(0x406b02);
		return click_wait_round(0x406afb);
	}
	X86_COUNT(1);
	return pop32();
}
