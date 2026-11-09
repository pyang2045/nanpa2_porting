/*
 * The script VM's own loop (R2: direct style): the entry 0x4060d4, the loop
 * 0x4060f9 with the click handlers, the opcode fetch and dispatch (0x40616e),
 * blocks (01 ... 00 / 02), text (06 ... 06 and Big5 characters), and the waits
 * the loop and the handlers use: the input test 0x4074d5, the frame wait
 * 0x4075c6 and the click wait 0x406acc.
 *
 * Every pass of the loop pumps messages (0x401bf0), steps the animations
 * (0x403624), tests the left and right button for the click handlers that
 * ext op 20 sets (0x4074d5 twice, which pumps again), then fetches one
 * opcode and runs its handler. This used to be a set of pieces that returned
 * native addresses to a dispatcher; it is ordinary C now:
 *
 *   - `vm_pc` is the program counter (the native ESI): an offset in segment 0.
 *   - vm_loop() runs until a block ends; a nested block (if, while, label and
 *     offset calls, the menu's blocks, a click handler, a called MES file) is a
 *     nested call of vm_loop(), as natively it was `call 0x4060f9` on the
 *     native stack. vm_depth counts them (the native test ESP == [top ESP]).
 *   - A handler returns -1 (go on with the next pass: the native `jmp
 *     0x4060f9`) or the block result AL (0 end, 1 continue, 2 break): the native
 *     `ret` out of the running block.
 *   - A wait is a C loop that pumps through ctl_pump(). WM_QUIT, the end of
 *     the script at the top level and opcodes that end the VM unwind with
 *     ctl_quit() to game_vm_run(); a restart (04 1e, 04 21) longjmps back to
 *     the top of the loop.
 *
 * Cost and polls are exactly the native pieces' (src/game/ctl.h): every
 * COST/POLL keeps its place relative to the pump and the other HLE calls.
 *
 * Handlers that have not been converted yet (the subsystem ops of ops_sys.c)
 * are still register-level pieces: legacy() runs one on the emulated CPU until
 * it jumps back to the loop head, with ESI = vm_pc.
 */
#include <setjmp.h>

#include "game/ctl.h"
#include "game/file.h"
#include "game/game.h"
#include "game/vm.h"
#include "hle/hle.h"
#include "x86/x86.h"
#include "game/state.h"

#define INPUT_SKIP 0x40u      /* Ctrl: no waiting for clicks */
#define BIG5_NEWLINE 0xa1beu

uint32_t vm_pc;
int vm_depth;
static jmp_buf *vm_restart_jmp;

/* ---- the entry, and the end */

/* a restart of the top-level loop from anywhere (04 1e, 04 21): the native
 * `mov esp, [top]` and jump to the loop head */
_Noreturn void vm_restart(void)
{
	longjmp(*vm_restart_jmp, 1);
}

static void vm_loop_top(void *arg)
{
	(void)arg;
	jmp_buf here;
	vm_restart_jmp = &here;
	setjmp(here);
	vm_depth = 0;
	vm_loop();
	x86_trap(0x4061aa, "a block result at the top level of the VM");
}

/* 0x4060d4 after its set-up: load START.MES (or the file named at
 * VM_SCRIPT_NAME) and run the VM until the script ends or the game quits. The
 * native tail (`mov esp, [top]`, pop ebp, ret: 3 instructions, the first
 * charged by ctl_quit) is charged here. */
void game_vm_run(void)
{
	vm_init();
	COST(3);
	game_file_load(VM_SCRIPT_NAME, VM_SCRIPT);
	vm_pc = rd16(VM_SCRIPT);
	COST(2);
	ctl_catch(vm_loop_top, NULL);
	COST(2);
}

/* The piece that WinMain (startup.c) enters by call, until it calls
 * game_vm_run directly: the native frame the piece-world expects. */
uint32_t game_vm_entry(void)
{
	push32(cpu.r[EBP]);
	RT_SET(top_esp, cpu.r[ESP]);
	COST(3);
	game_vm_run();
	cpu.r[ESP] = RT(top_esp);
	cpu.r[EBP] = pop32();
	return pop32();
}

/* ---- the still register-level handlers */

/* Run the piece-world handler at native address `addr` (entered with ESI past
 * the opcode bytes, EAX = its table index) until it jumps to the loop head.
 * Its pieces call HLE routines and each other as the dispatcher did: units by
 * address, anything else through hle_call with the return address on the
 * stack. A quit leaves through the native exits, which unwind like ctl_quit. */
static void legacy(uint32_t addr, uint32_t index)
{
	cpu.r[ESI] = vm_pc;
	cpu.r[EAX] = index;
	uint32_t pc = addr;
	for (;;) {
		if (pc == VM_LOOP)
			break;
		if (pc == 0x40638d)
			ctl_quit();
		if (pc == 0x406393 || pc == 0x40472f)
			ctl_unwind();
		const struct game_unit *u = game_unit_find(pc);
		if (u && u->jump) {
			uint32_t next = u->jump();
			if (u->poll_self && next == pc)
				POLL(pc);
			pc = next;
			continue;
		}
		uint32_t ra = rd32(cpu.r[ESP]);
		hle_call(pc);
		pc = ra;
	}
	vm_pc = cpu.r[ESI];
}

/* ---- waits */

/* 0x4074d5, AL = k: pump, then test input k: 0 click/Enter (0x10), 1 right
 * click/Esc (0x20), 2-5 up, down, left, right (1, 2, 4, 8). 1 when it is down
 * (the native ZF clear). WM_QUIT ends the VM. */
int vm_input_down(uint32_t k)
{
	static const uint8_t bit[6] = { 0x10, 0x20, 0x01, 0x02, 0x04, 0x08 };
	COST(1); /* push eax */
	COST(1); /* call 0x401bf0 */
	int alive = ctl_pump();
	COST(2);
	if (!alive)
		ctl_quit();
	k &= 0xff;
	COST(1 + 2 * (k < 6 ? k + 1 : 6));
	int zf;
	if (k < 6) {
		zf = (RT(input_bits) & bit[k]) == 0;
		COST(2);
	} else {
		zf = k == 5; /* the last `cmp al, 5` */
		COST(1);
	}
	return !zf;
}

/* 0x4075c6, n: wait until the frame counter reaches its value now + n / 4 + 1,
 * pumping and stepping the animations each round */
void vm_wait_frames(uint32_t n)
{
	uint32_t target = RT(frame) + (n >> 2) + 1;
	COST(5);
	COST(1); /* call 0x401bf0 */
	for (;;) {
		int alive = ctl_pump();
		COST(2);
		if (!alive)
			ctl_quit();
		COST(1);
		game_anim_step();
		COST(3);
		if (target > RT(frame)) { /* ja: not there yet */
			POLL(0x4075eb);
			COST(1); /* push eax */
			COST(1); /* call 0x401bf0 */
			continue;
		}
		COST(1);
		return;
	}
}

/* 0x406acc / 0x406ae8: with Ctrl down the wait ends at once (1), else wait a
 * frame (0) */
static int click_skip_or_frame(void)
{
	int ctrl = (RT(input_bits) & INPUT_SKIP) != 0;
	COST(3);
	if (ctrl) {
		COST(1);
		return 1;
	}
	COST(1); /* mov ecx, 1 */
	COST(1); /* call 0x4075c6 */
	vm_wait_frames(1);
	return 0;
}

/* 0x406acc: wait for a click or Enter, pressed (one frame per test) and then
 * released */
void vm_wait_click(void)
{
	for (;;) {
		if (click_skip_or_frame())
			return;
		COST(1); /* mov al, 0 */
		COST(1); /* call 0x4074d5 */
		int down = vm_input_down(0);
		COST(1);
		if (down)
			break;
		POLL(0x406ae6);
	}
	for (;;) {
		if (click_skip_or_frame())
			return;
		COST(1);
		COST(1);
		int down = vm_input_down(0);
		COST(1);
		if (!down) {
			COST(1);
			return;
		}
		POLL(0x406b02);
	}
}

/* ---- blocks */

/* 0x4061cd, called with vm_pc at a block's 01: run the block as a nested loop
 * (`call 0x4060f9`); the result is its AL. The caller charges its own `call`. */
int vm_run_block(void)
{
	vm_pc++;
	COST(1);
	COST(1); /* call 0x4060f9 */
	vm_depth++;
	int al = vm_loop();
	vm_depth--;
	COST(1); /* ret of 0x4061cd */
	return al;
}

/* a nested loop at vm_pc (`call 0x4060f9`): the result is its AL */
int vm_call_loop(void)
{
	COST(1);
	vm_depth++;
	int al = vm_loop();
	vm_depth--;
	return al;
}

/* 00 or 02 (0x4061aa): the end of the running block, or of the VM at the top
 * level; a block returns with AL = 0 */
static int block_end(void)
{
	vm_pc++;
	COST(4);
	if (vm_depth == 0)
		ctl_quit();
	COST(2);
	return 0;
}

/* 01 (0x4061bc): run the block (a nested loop) with vm_pc saved; then skip
 * it (0x4061d4) and go on */
static int block_open(void)
{
	uint32_t saved = vm_pc;
	COST(1);
	COST(1); /* call 0x4061cd */
	vm_run_block();
	vm_pc = vm_skip_block(saved);
	COST(3);
	POLL(0x4061c8);
	return -1;
}

/* ---- text */

/* the character delay after a character */
static void char_delay(void)
{
	uint32_t n = SV(text_delay);
	COST(3);
	if (!n)
		return;
	COST(1); /* call 0x4075c6 */
	vm_wait_frames(n);
}

/* the text colour as the native code pushes it: bg = colour >> 4 (all of
 * it), fg = its low nibble */
static uint32_t colour_bg(void)
{
	return SV(text_colour) >> 4;
}

static uint32_t colour_fg(void)
{
	return SV(text_colour) & 0xf;
}

/* 0x4062bf: the characters of a 06 string: 06 ends it, any other byte is one
 * ASCII character (size 0) at the cursor */
static void string_chars(void)
{
	for (;;) {
		uint32_t c = vm_script_byte(vm_pc++);
		COST(4);
		if (c == VM_STRING) {
			POLL(0x4062c8);
			return;
		}
		uint32_t bg = colour_bg(), fg = colour_fg(), y = SV(text_y), x = SV(text_x) << 3;
		COST(15);
		x86_probe(0x4062ff);
		game_draw_char(c, x, y, 0, fg, bg);
		char_delay();
		COST(1);
		game_text_advance();
		COST(1);
		POLL(0x40631a);
	}
}

/* a byte from 0x80 (0x40631c) and the next: one Big5 character, A1BE a
 * newline */
static void big5_char(uint32_t first)
{
	uint32_t pc = vm_pc + 1;
	uint32_t c = (first & 0xff) << 8 | vm_script_byte(pc++);
	vm_pc = pc;
	COST(6);
	if (c == BIG5_NEWLINE) {
		COST(1);
		game_text_newline();
		COST(1);
		POLL(0x406331);
		return;
	}
	uint32_t bg = colour_bg(), fg = colour_fg(), size = SV(text_size), y = SV(text_y),
	         x = SV(text_x) << 3;
	COST(16);
	x86_probe(0x40636d);
	game_draw_char(c, x, y, size, fg, bg);
	char_delay();
	COST(1);
	game_text_advance();
	COST(1);
	POLL(0x406388);
}

/* ---- fetch and dispatch */

/* handlers of ops 0a-1a (table 0x40620b; 10 is a no-op, the loop head) and of
 * the extended ops 04 10-04 26 (table 0x406262): the C handler, or for the ones
 * still register-level (fn NULL) its native address as an offset from
 * HANDLER_BASE (16 bits, so the binary does not hold the native tables: ctest
 * no_game_bytes) */
#define HANDLER_BASE 0x400000u
struct handler {
	int (*fn)(void);
	uint16_t native;
};
static const struct handler op_handlers[] = {
	{ vm_op_set_flags, 0 }, { vm_op_set_flags_expr, 0 }, { vm_op_set_reg, 0 }, { vm_op_store_words, 0 },
	{ vm_op_store_bytes, 0 }, { vm_op_if, 0 }, { NULL, 0x60f9 }, { vm_op_wait, 0 }, { vm_op_set_label, 0 },
	{ vm_op_call_label, 0 }, { vm_op_call_offset, 0 }, { vm_op_print_number, 0 }, { vm_op_delay, 0 },
	{ NULL, 0x6b05 }, { vm_op_text_colour, 0 }, { NULL, 0x7204 }, { NULL, 0x7096 },
};
static const struct handler ext_handlers[] = {
	{ vm_op_while, 0 }, { vm_op_continue, 0 }, { vm_op_break, 0 }, { vm_op_menu, 0 }, { NULL, 0x6842 },
	{ NULL, 0x6916 }, { NULL, 0x6f06 }, { NULL, 0x6b76 }, { NULL, 0x6bcb }, { NULL, 0x6c20 },
	{ NULL, 0x6c85 }, { NULL, 0x6cea }, { NULL, 0x6d8b }, { NULL, 0x6e11 }, { vm_op_jump_mes, 0 },
	{ vm_op_call_mes, 0 }, { vm_op_click_handler, 0 }, { vm_op_flag_file, 0 }, { vm_op_args_only, 0 },
	{ vm_op_hit_test, 0 }, { NULL, 0x73a5 }, { NULL, 0x73cd }, { NULL, 0x73ee },
};
#define N_OPS (sizeof op_handlers / sizeof op_handlers[0])
#define N_EXT (sizeof ext_handlers / sizeof ext_handlers[0])

static int run_handler(const struct handler *h, uint32_t index)
{
	if (h->fn)
		return h->fn();
	if (h->native == 0x60f9) /* op 10: the loop head itself, no backward jump */
		return -1;
	legacy(HANDLER_BASE + h->native, index);
	return -1;
}

/* 0x40616e: fire the fetch probe (the exploration bot and NANPA2_VMTRACE
 * watch it), read the opcode at vm_pc and run what it is. Returns -1 or the
 * block result. */
static int fetch(void)
{
	probe_vm_fetch(vm_pc);
	uint32_t op = vm_script_byte(vm_pc);
	COST(1);
	if (op == VM_BLOCK_END) {
		COST(2);
		return block_end();
	}
	if (op == VM_BLOCK_OPEN) {
		COST(4);
		return block_open();
	}
	if (op == 0x04) { /* 04 xx: extended op (no bound check natively) */
		uint32_t xx = vm_script_byte(vm_pc + 1);
		vm_pc += 2;
		uint32_t index = (xx - 0x10) & 0xffffffffu;
		COST(6 + 5);
		if (index >= N_EXT)
			x86_trap(0x40625b, "extended opcode outside the native table");
		return run_handler(&ext_handlers[index], index);
	}
	if (op == VM_SEPARATOR) {
		COST(8);
		return block_end();
	}
	if (op == VM_STRING) {
		vm_pc++;
		COST(10 + 1);
		string_chars();
		return -1;
	}
	if (op >= 0x80) {
		COST(12);
		big5_char(op);
		return -1;
	}
	if (op < 0x09 || op >= 0x20) { /* 03, 05, 07, 08 and 0x20-0x7f end the VM */
		COST(op < 0x09 ? 14 : 16);
		ctl_quit();
	}
	vm_pc++;
	uint32_t index = op - 0x0a;
	COST(17 + 4);
	if (index >= N_OPS)
		x86_trap(0x406204, "opcode outside the native table (09, 1b-1f)");
	return run_handler(&op_handlers[index], index);
}

/* ---- the loop */

/* 0x406143: a button is down with its handler set: wait until the left, then
 * the right button are up, then run the handler's block (busy flag set, vm_pc
 * saved) */
static int click_run(uint32_t handler)
{
	for (;;) {
		COST(1); /* mov al, 0 */
		COST(1); /* call 0x4074d5 */
		int down = vm_input_down(0);
		COST(1);
		if (down) {
			POLL(0x40614a);
			continue;
		}
		COST(1); /* mov al, 1 */
		COST(1);
		down = vm_input_down(1);
		COST(1);
		if (down) {
			POLL(0x406153);
			continue;
		}
		break;
	}
	uint32_t saved = vm_pc;
	RT_SET(click_busy, 0xff);
	vm_pc = handler;
	COST(3);
	COST(1); /* call 0x4061cd */
	vm_run_block();
	RT_SET(click_busy, 0);
	vm_pc = saved;
	COST(3); /* mov [busy], pop esi, jmp 0x4060f9 */
	POLL(0x40616c);
	return -1;
}

/* one pass of the loop (0x4060f9): pump, step the animations, test the buttons
 * for their click handlers unless they are off (@[9] bit 2) or one is
 * running, then fetch and run one opcode */
static int vm_pass(void)
{
	COST(1); /* call 0x401bf0 */
	int alive = ctl_pump();
	COST(2);
	if (!alive)
		ctl_quit();
	COST(1);
	game_anim_step();
	COST(2);
	if (SV(vflags) & 4)
		return fetch();
	COST(2);
	if (RT(click_busy))
		return fetch();
	COST(1); /* mov al, 0 */
	COST(1); /* call 0x4074d5 */
	int down = vm_input_down(0);
	uint32_t h = rd16(VM_CLICK_HANDLERS);
	COST(2);
	if (down) {
		COST(2); /* or edi, edi */
		if (!h)
			return fetch();
		return click_run(h);
	}
	COST(1); /* mov al, 1 */
	COST(1);
	down = vm_input_down(1);
	h = rd16(VM_CLICK_HANDLERS + 2);
	COST(2);
	if (down) {
		COST(2);
		if (!h)
			return fetch();
		return click_run(h);
	}
	COST(1);
	return fetch();
}

/* 0x4060f9: run until a block ends; the block result (AL) */
int vm_loop(void)
{
	for (;;) {
		int r = vm_pass();
		if (r >= 0)
			return r;
	}
}
