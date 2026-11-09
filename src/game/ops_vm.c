/*
 * Opcode handlers that run a block, a script or a wait (R2: direct style).
 *
 * The native handlers of if (0f), while (04 10), call label (13), call offset
 * (14), wait (11, 16), the menu loop (04 13), the MES-file ops (04 1e, 04 1f,
 * 04 21) and the number printer (15) call into the VM itself (0x4061cd runs a
 * block, 0x4060f9 a script from ESI) or into routines that pump messages and
 * wait on the timer. They were jump units split at each such call; they are
 * ordinary C functions now: a block or script is a nested call of vm_loop()
 * (src/game/loop.c), a wait a C loop that pumps, and what the native handler
 * kept on its stack (the statement's pc) is a local.
 *
 * Contract of a handler: it is entered with vm_pc past the opcode bytes and
 * returns -1 (on with the loop) or the block result AL (0 end, 1 continue, 2
 * break) that the native `ret` out of the running block leaves.
 *
 * Semantics: re-notes.md "Script opcodes".
 */
#include "game/ctl.h"
#include "game/file.h"
#include "game/vm.h"
#include "x86/x86.h"
#include "game/state.h"

/* `jmp 0x4060f9` (or a jcc) at native_pc: backward, so it polls */
static int loop_at(uint32_t native_pc)
{
	POLL(native_pc);
	return -1;
}

/* call 0x4061d4: skip the block at pc; the depth counter is gone (AX = 0) */
static uint32_t skip(uint32_t pc)
{
	pc = vm_skip_block(pc);
	COST(1);
	return pc;
}

/* 0f expr 01 ... 00 [02 0f expr 01 ... 00]... [02 01 ... 00]: if, else
 * if, else. The condition is the low word of the value (`cmp ax, 0`). A
 * true condition runs its block, then skips the else parts; a false one skips
 * the block, then 02 0f goes back to the entry for the next condition, 02
 * alone runs the else block, anything else ends the statement. A block that
 * ended with continue or break (AL 1 or 2) returns from the running block
 * with it. */
int vm_op_if(void)
{
	for (;;) {
		uint32_t pc = vm_pc;
		uint32_t v = vm_eval(&pc);
		COST(3);
		if (v & 0xffff) {
			vm_pc = pc;
			COST(1); /* call 0x4061cd */
			int al = vm_run_block();
			/* 0x4065ae: skip one else part (02 + block) per round */
			for (;;) {
				pc = vm_pc;
				COST(2);
				if (vm_script_byte(pc) != VM_SEPARATOR)
					break;
				vm_pc = skip(pc);
				COST(1);
				POLL(0x4065c5);
				al = 0;
			}
			COST(1);
			if (!(al & 0xff))
				return loop_at(0x4065b9);
			COST(1);
			return al;
		}
		pc = skip(pc);
		COST(3);
		if (vm_script_byte(pc) != VM_SEPARATOR) {
			vm_pc = pc;
			return loop_at(0x4065d5);
		}
		pc++;
		COST(2);
		if (vm_script_byte(pc) == VM_IF) {
			vm_pc = pc + 1;
			COST(2);
			POLL(0x4065e6);
			continue; /* the handler's entry: the next condition */
		}
		vm_pc = pc;
		COST(1); /* call 0x4061cd */
		int al = vm_run_block();
		COST(2); /* 0x4065ed: after the else block */
		if (!(al & 0xff))
			return loop_at(0x4065ef);
		COST(1);
		return al;
	}
}

/* 04 10 x expr 01 ... 00: while. The statement's offset is saved; the byte
 * after 04 10 is skipped; a true condition (low word) runs the block, which
 * ends the loop with break (AL = 2) or goes round again, a false one skips
 * the block and ends the loop. */
int vm_op_while(void)
{
	uint32_t start = vm_pc;
	for (;;) {
		uint32_t pc = start + 1;
		uint32_t v = vm_eval(&pc);
		COST(5);
		if (!(v & 0xffff)) {
			vm_pc = skip(pc);
			COST(1);
			return loop_at(0x406633);
		}
		vm_pc = pc;
		COST(1); /* call 0x4061cd */
		int al = vm_run_block();
		vm_pc = start;
		COST(3);
		if ((al & 0xff) != 2) {
			POLL(0x406621);
			continue;
		}
		vm_pc = skip(start);
		return loop_at(0x406628);
	}
}

/* 13 n: run the block at label n (0x445dbc + 2n), then go on after the
 * statement. */
int vm_op_call_label(void)
{
	uint32_t a;
	vm_parse_args(&vm_pc, &a);
	uint32_t saved = vm_pc;
	vm_pc = rd16(VM_LABELS + 2 * a);
	COST(3);
	COST(1); /* call 0x4061cd */
	vm_run_block();
	vm_pc = saved;
	COST(1);
	return loop_at(0x40666c);
}

/* 14 off: run the script at offset off (a nested VM, call 0x4060f9) up to
 * its 00/02, then go on after the statement. */
int vm_op_call_offset(void)
{
	uint32_t a;
	vm_parse_args(&vm_pc, &a);
	uint32_t saved = vm_pc;
	vm_pc = a;
	COST(3);
	vm_call_loop();
	vm_pc = saved;
	COST(1);
	return loop_at(0x406aa8);
}

/* 11: wait for a click (0x406acc), then clear the text window in the text
 * colour. */
int vm_op_wait(void)
{
	COST(1); /* call 0x406acc */
	vm_wait_click();
	COST(2);
	game_clear_text_window(SV(text_colour)); /* 0x406b24: draws, no waiting */
	return loop_at(0x406607);
}

/* 16 [n]: wait n/4 + 1 frames (0x4075c6), or with no argument for a click
 * (0x406acc); then go on. */
int vm_op_delay(void)
{
	uint32_t a, n = vm_parse_args(&vm_pc, &a);
	COST(2);
	if (n) {
		COST(1);
		COST(1); /* call 0x4075c6 */
		vm_wait_frames(a);
		COST(1);
		return loop_at(0x406abd);
	}
	COST(1); /* call 0x406acc */
	vm_wait_click();
	COST(1);
	return loop_at(0x406ac7);
}

/* ---- calling into the routines that load files */

static void load(uint32_t name, uint32_t dst)
{
	COST(2);
	game_file_load_at(name, dst);
	COST(1);
}

/* rep movsd of n dwords from src to dst */
static void movsd(uint32_t n, uint32_t src, uint32_t dst)
{
	for (uint32_t i = 0; i < n; i++)
		wr32(dst + 4 * i, rd32(src + 4 * i));
	COST(n);
}

#define MES_NAME 0x444e78u   /* name of the running MES file (32 bytes) */
#define FLAG_NAME 0x445d86u  /* "FLAGn" (n at 0x445d8a) */

/* 04 1e and the end of 04 21 0: load the MES file named at MES_NAME over
 * the script, reset the animation slots and the click handlers, and restart
 * the VM at the file's entry on the top-level stack. */
static int restart(void)
{
	load(MES_NAME, VM_SCRIPT);
	game_anim_reset();
	RT_SET(click_busy, 0);
	wr16(VM_CLICK_HANDLERS, 0);
	wr16(VM_CLICK_HANDLERS + 2, 0);
	vm_pc = rd16(VM_SCRIPT);
	COST(6);
	/* from a nested VM the stack drops back to the top level: the native
	 * handler leaves for 0x406fc8 (`jmp 0x4060f9`), one more instruction */
	if (vm_depth > 0) {
		COST(1);
		POLL(0x406fc8);
		vm_restart();
	}
	return loop_at(0x406fc8);
}

/* 04 1e name: jump to another MES file. */
int vm_op_jump_mes(void)
{
	vm_parse_args(&vm_pc, &(uint32_t){ 0 });
	uint32_t src, last;
	vm_string_arg(MES_NAME, &src, &last);
	COST(3);
	return restart();
}

/* 04 21 mode, n: the FLAGn file. mode 0 loads it over the VM state (from
 * MES_NAME on: name, flags, registers ... 0xc00 bytes) and restarts in the
 * MES file it names; 1 saves those 0xc00 bytes; anything else loads it but
 * keeps the running file's name. A load keeps bits 5-6 of @[9] and redoes
 * the VM setup (0x406395). */
static void flag_load(void)
{
	uint32_t keep = SV(vflags) & 0x60;
	COST(3);
	load(FLAG_NAME, MES_NAME);
	uint32_t f = (SV(vflags) & 0x9f) | keep;
	SV_SET(vflags, f);
	COST(5);
	vm_init();
	COST(1);
}

int vm_op_flag_file(void)
{
	uint32_t mode;
	vm_parse_args(&vm_pc, &mode);
	uint32_t bx = (rd16(VM_ARG_VALUES + 2) + 0x30) & 0xff;
	wr8(FLAG_NAME + 4, bx);
	COST(5);
	if (mode == 0) {
		flag_load();
		return restart();
	}
	COST(2);
	if (mode == 1) {
		COST(3);
		game_file_save_at(FLAG_NAME, MES_NAME, 0xc00);
		COST(1);
		return loop_at(0x40719f);
	}
	uint32_t pc = vm_pc;
	movsd(8, MES_NAME, 0x445d66);
	flag_load();
	movsd(8, 0x445d66, MES_NAME);
	vm_pc = pc;
	COST(8);
	return loop_at(0x40714d);
}

/* 0x4069fd: format AX as decimal at 0x445ce6 (EDI), for op 15. DL = the
 * number of digits (0: as many as the value has, 1-4: that many low
 * digits, else 5); DH != 0: ASCII between 06 marks, else Big5 full-width
 * digits (0xa2af + d) and a NUL. The first digit counts how often its
 * place value fits, so a value too big for the digits gives a "digit"
 * above 9, as natively. Out: AX = 0, BX = the last digit's code, CX = 1,
 * EDI past the digits (DI only, for ASCII), DL/DH kept. */
static void format_number(uint32_t value, uint32_t dl, uint32_t dh)
{
	static const uint16_t place[5] = { 10000, 1000, 100, 10, 1 };
	uint32_t edi = 0x445ce6, ax = value & 0xffff, bx = 0;
	COST(3);
	if (dh) {
		wr8(edi++, VM_STRING);
		COST(2);
	}
	int first;
	if (dl == 0)
		first = ax >= 10000 ? 0 : ax >= 1000 ? 1 : ax >= 100 ? 2 : ax >= 10 ? 3 : 4;
	else
		first = dl == 1 ? 4 : dl == 2 ? 3 : dl == 3 ? 2 : dl == 4 ? 1 : 0;
	COST(2 * (uint32_t)(5 - first) + 2);
	for (int i = first; i < 5; i++) {
		uint32_t cx = place[i];
		bx &= 0xffff0000u;
		COST(3);
		while (ax >= cx) {
			ax -= cx;
			bx = (bx & 0xffff0000u) | ((bx + 1) & 0xffff);
			COST(3);
			POLL(0x406a70);
		}
		COST(5);
		if (!dh) {
			bx = (bx & 0xffff0000u) | ((bx + 0xa2af) & 0xffff);
			wr8(edi, bx >> 8);
			wr8(edi + 1, bx);
			edi += 2;
			wr8(edi, 0);
		} else {
			bx = (bx & ~0xffu) | ((bx + 0x30) & 0xff);
			wr8(edi, bx);
			edi = (edi & 0xffff0000u) | ((edi + 1) & 0xffff);
			wr8(edi, VM_STRING);
			wr8(edi + 1, 0);
		}
		COST(5);
	}
}

#define NUMBER_TEXT 0x445ce6u   /* 0x4069fd writes the digits here */

/* 15 v: print v as text: format it (0x4069fd), then run the digits as a
 * script (a nested VM at NUMBER_TEXT, which draws them like any text)
 * and go on. */
int vm_op_print_number(void)
{
	uint32_t a;
	vm_parse_args(&vm_pc, &a);
	uint32_t f = SV(number_format);
	COST(5);
	format_number(a, f & 0x0f, (f & 0xf0) >> 4 ? 1 : 0);
	uint32_t saved = vm_pc;
	vm_pc = NUMBER_TEXT - VM_SCRIPT;
	COST(4);
	vm_call_loop();
	vm_pc = saved;
	COST(1);
	return loop_at(0x4069f8);
}

/* 04 1f name: run another MES file as a subroutine. The labels (0x1e
 * dwords from VM_LABELS) and the file name are saved, the file is loaded
 * over the script, the click handler state is saved and cleared, and the
 * file runs as a nested VM from its entry; then the click handlers, the
 * labels and the name are restored (the native `mov cx, 0x1e` leaves ECX's
 * upper half as the nested VM left it, zero from the message pump: the count
 * is 0x1e), the caller's file is loaded again and the script goes on. */
#define SAVED_LABELS 0x445e36u
#define SAVED_NAME 0x445d46u

int vm_op_call_mes(void)
{
	vm_parse_args(&vm_pc, &(uint32_t){ 0 });
	uint32_t saved = vm_pc;
	movsd(0x1e, VM_LABELS, SAVED_LABELS);
	movsd(8, MES_NAME, SAVED_NAME);
	uint32_t src, last;
	vm_string_arg(MES_NAME, &src, &last);
	COST(8);
	load(MES_NAME, VM_SCRIPT);
	uint32_t busy = RT(click_busy), left = rd16(VM_CLICK_HANDLERS), right = rd16(VM_CLICK_HANDLERS + 2);
	RT_SET(click_busy, 0);
	wr16(VM_CLICK_HANDLERS, 0);
	wr16(VM_CLICK_HANDLERS + 2, 0);
	vm_pc = rd16(VM_SCRIPT);
	COST(10);
	vm_call_loop();
	wr16(VM_CLICK_HANDLERS + 2, right);
	wr16(VM_CLICK_HANDLERS, left);
	RT_SET(click_busy, busy);
	COST(6);
	movsd(0x1e, SAVED_LABELS, VM_LABELS);
	movsd(8, SAVED_NAME, MES_NAME);
	COST(6);
	load(MES_NAME, VM_SCRIPT);
	vm_pc = saved;
	COST(1);
	return loop_at(0x407091);
}

/* ---- 04 13 [n]: the menu / event loop (0x406671)
 *
 * The block at label n (or the inline block given as the argument) is the
 * menu (0x445e0c, for 04 14); label 38 runs first, then each round: pump
 * (0x401bf0; WM_QUIT ends the VM), step the animations (0x403624), and
 * while [0x445304] stays set, run label 39, then for the first input k =
 * 0..5 enabled in 0x4452e1 (bit 1 << (k + 1): click, right click, the arrows)
 * that is down (0x4074d5 k) run label 32 + k and, unless bit 0 of
 * 0x4452e1 is set, wait until k is released (stepping the animations
 * meanwhile for k = 5); then the next round. */
#define MENU_BLOCK 0x445e0cu /* u16: the menu's block */
#define MENU_INIT 0x445e08u  /* label 38 */
#define MENU_FRAME 0x445e0au /* label 39 */

static const struct menu_input {
	uint32_t bit, handler;
	uint32_t pc_pump_after_block, pc_wait_loop, pc_pump_after_wait; /* native backward jumps */
} menu_inputs[6] = {
	{ 0x02, 0x445dfc, 0x40670b, 0x406714, 0x406716 },
	{ 0x04, 0x445dfe, 0x40673d, 0x40674a, 0x40674c },
	{ 0x08, 0x445e00, 0x406776, 0x406783, 0x406785 },
	{ 0x10, 0x445e02, 0x4067af, 0x4067bc, 0x4067be },
	{ 0x20, 0x445e04, 0x4067e8, 0x4067f5, 0x4067f7 },
	{ 0x40, 0x445e06, 0x406829, 0x40683b, 0x40683d },
};

int vm_op_menu(void)
{
	vm_parse_args(&vm_pc, &(uint32_t){ 0 });
	uint32_t type = rd8(VM_ARG_TYPES);
	COST(3);
	uint32_t blk;
	if (type == VM_ARG_BLOCK) {
		blk = rd16(VM_ARG_VALUES);
	} else {
		blk = rd16(VM_LABELS + 2 * rd16(VM_ARG_VALUES));
		COST(2);
	}
	wr16(MENU_BLOCK, blk);
	SV_SET(menu_run, 1);
	uint32_t saved = vm_pc;
	vm_pc = rd16(MENU_INIT);
	COST(5);
	COST(1); /* call 0x4061cd */
	vm_run_block();
	uint32_t poll = 0; /* the native backward jump to the round's start, 0 for the first round */
	for (;;) {
		if (poll)
			POLL(poll);
		poll = 0;
		COST(1); /* call 0x401bf0 */
		int alive = ctl_pump();
		COST(2);
		if (!alive)
			ctl_quit();
		game_anim_step();
		COST(2);
		if (!SV(menu_run)) {
			vm_pc = saved;
			COST(1);
			return loop_at(0x4066d5);
		}
		vm_pc = rd16(MENU_FRAME);
		COST(1);
		COST(1); /* call 0x4061cd */
		vm_run_block();
		poll = 0x406803; /* the scan ran off the end */
		for (int k = 0; k < 6; k++) {
			COST(2);
			if (!(SV(input_enable) & menu_inputs[k].bit))
				continue;
			COST(1); /* mov al, k */
			COST(1); /* call 0x4074d5 */
			int down = vm_input_down((uint32_t)k);
			COST(1);
			if (!down) {
				if (k < 5)
					continue;
				poll = 0x406810;
				break;
			}
			vm_pc = rd16(menu_inputs[k].handler);
			COST(1);
			COST(1); /* call 0x4061cd */
			vm_run_block();
			COST(2);
			if (SV(input_enable) & 1) {
				poll = menu_inputs[k].pc_pump_after_block;
				break;
			}
			for (;;) { /* wait for the release */
				if (k == 5)
					game_anim_step();
				COST(1); /* mov al, k */
				COST(1); /* call 0x4074d5 */
				down = vm_input_down((uint32_t)k);
				COST(1);
				if (!down)
					break;
				POLL(menu_inputs[k].pc_wait_loop);
			}
			poll = menu_inputs[k].pc_pump_after_wait;
			break;
		}
	}
}
