/*
 * Opcode handlers that run a block, a script or a wait (jump units split
 * at their calls, stage 3).
 *
 * The native handlers of if (0f), while (04 10), call label (13), call
 * offset (14), wait (11, 16) and the menu loop (04 13) call into the VM
 * itself (0x4061cd runs a block, 0x4060f9 a script from ESI) or into
 * routines that pump messages and wait on the timer. Their C versions do
 * not run those: at such a call they do what the native `call` does, push
 * the return address and give the dispatcher the callee's address
 * (call_exit). The block, script or wait then runs natively, once, on the
 * emulated stack, and returns to the return address, where the rest of the
 * handler is a jump unit of its own (game_op_if_then is 0f after its
 * block). No host frames are left under a running block, so the VM's
 * stack resets (04 1e, the quit path 0x40638d) need nothing special, and
 * under the differential test a piece's oracle ends at the same call
 * (HLE_GAME_CALL_EXIT, src/hle/overrides.h), so no block or wait runs
 * twice.
 *
 * Each piece's contract: in, the registers the native code there reads
 * (ESI, and after a block AL, its result: 0 end, 1 continue, 2 break);
 * out, ESI and, where the piece can `ret` out of the running block, AL.
 * A jump back to the handler's own entry or to another piece ends a piece
 * too (the dispatcher goes on there), as it ends the oracle.
 *
 * Semantics: re-notes.md "Script opcodes".
 */
#include "game/vm.h"
#include "x86/x86.h"

#define RUN_BLOCK 0x4061cdu  /* inc esi; call 0x4060f9: the block at ESI (its 01) */
#define WAIT_CLICK 0x406acc  /* wait for a click or Enter, pressed then released */
#define WAIT_FRAMES 0x4075c6 /* ECX/4 + 1 frames, pumping and animating */

/* the native `call target` with return address ra */
static uint32_t call_exit(uint32_t target, uint32_t ra)
{
	push32(ra);
	X86_COUNT(1);
	return target;
}

/* `jmp 0x4060f9` (or a jcc) at native_pc: backward, so it polls */
static uint32_t loop_at(uint32_t native_pc)
{
	X86_POLL(native_pc);
	return VM_LOOP;
}

/* call 0x4061d4: skip the block at pc; AX = 0 (the depth counter) */
static uint32_t skip(uint32_t pc)
{
	pc = vm_skip_block(pc);
	cpu.r[EAX] &= 0xffff0000u;
	X86_COUNT(1);
	return pc;
}

/* 0f expr 01 ... 00 [02 0f expr 01 ... 00]... [02 01 ... 00]: if, else
 * if, else. The condition is the low word of the value (`cmp ax, 0`). A
 * true condition runs its block (on to game_op_if_then); a false one skips
 * it, then 02 0f goes back to the entry for the next condition, 02 alone
 * runs the else block (on to game_op_if_else), anything else ends the
 * statement with AL = 0. */
uint32_t game_op_if(void)
{
	uint32_t pc = cpu.r[ESI];
	uint32_t v = vm_eval(&pc);
	cpu.r[EAX] = v;
	X86_COUNT(3);
	if (v & 0xffff) {
		cpu.r[ESI] = pc;
		return call_exit(RUN_BLOCK, 0x4065ae);
	}
	pc = skip(pc);
	cpu.r[EAX] &= ~0xffu; /* mov al, 0 */
	X86_COUNT(3);
	if (vm_script_byte(pc) != VM_SEPARATOR) {
		cpu.r[ESI] = pc;
		return loop_at(0x4065d5);
	}
	pc++;
	X86_COUNT(2);
	if (vm_script_byte(pc) == VM_IF) {
		cpu.r[ESI] = pc + 1;
		X86_COUNT(2);
		X86_POLL(0x4065e6);
		return 0x40659e;
	}
	cpu.r[ESI] = pc;
	return call_exit(RUN_BLOCK, 0x4065ed);
}

/* 0f after a true condition's block: skip one else part (02 + block)
 * per round, coming back here (the native loop jumps back to 0x4065ae);
 * then, with AL as the block left it (0 after a skip), AL = 0 goes on with
 * the script and 1/2 (continue/break) return from the running block. */
uint32_t game_op_if_then(void)
{
	uint32_t pc = cpu.r[ESI];
	X86_COUNT(2);
	if (vm_script_byte(pc) == VM_SEPARATOR) {
		cpu.r[ESI] = skip(pc);
		X86_COUNT(1);
		X86_POLL(0x4065c5);
		return 0x4065ae;
	}
	X86_COUNT(1);
	if (!(cpu.r[EAX] & 0xff))
		return loop_at(0x4065b9);
	X86_COUNT(1);
	return pop32();
}

/* 0f after the else block: AL = 0 goes on, else return from the running
 * block with it. */
uint32_t game_op_if_else(void)
{
	X86_COUNT(2);
	if (!(cpu.r[EAX] & 0xff))
		return loop_at(0x4065ef);
	X86_COUNT(1);
	return pop32();
}

/* 04 10 x expr 01 ... 00: while. The statement's offset is saved on the
 * stack; the byte after 04 10 is skipped; a true condition (low word)
 * runs the block (on to game_op_while_next), a false one skips it and
 * ends the loop (EAX = the saved offset, popped). */
uint32_t game_op_while(void)
{
	uint32_t start = cpu.r[ESI], pc = start + 1;
	push32(start);
	uint32_t v = vm_eval(&pc);
	cpu.r[EAX] = v;
	X86_COUNT(5);
	if (v & 0xffff) {
		cpu.r[ESI] = pc;
		return call_exit(RUN_BLOCK, 0x40661e);
	}
	cpu.r[ESI] = skip(pc);
	cpu.r[EAX] = pop32();
	X86_COUNT(1);
	return loop_at(0x406633);
}

/* 04 10 after its block: back to the statement (the entry) unless the
 * block ended with break (AL = 2), which skips the block and goes on. */
uint32_t game_op_while_next(void)
{
	uint32_t start = pop32();
	cpu.r[ESI] = start;
	X86_COUNT(3);
	if ((cpu.r[EAX] & 0xff) != 2) {
		X86_POLL(0x406621);
		return 0x40660c;
	}
	cpu.r[ESI] = skip(start);
	return loop_at(0x406628);
}

/* 13 n: run the block at label n (0x445dbc + 2n, EAX as the parser left
 * it), then go on after the statement (game_op_call_label_done). */
uint32_t game_op_call_label(void)
{
	game_parse_args_regs();
	push32(cpu.r[ESI]);
	cpu.r[ESI] = rd16(VM_LABELS + 2 * cpu.r[EAX]);
	X86_COUNT(3);
	return call_exit(RUN_BLOCK, 0x40666b);
}

uint32_t game_op_call_label_done(void)
{
	cpu.r[ESI] = pop32();
	X86_COUNT(1);
	return loop_at(0x40666c);
}

/* 14 off: run the script at offset off (a nested VM, call 0x4060f9) up to
 * its 00/02, then go on after the statement. */
uint32_t game_op_call_offset(void)
{
	game_parse_args_regs();
	push32(cpu.r[ESI]);
	cpu.r[ESI] = cpu.r[EAX];
	X86_COUNT(3);
	return call_exit(VM_LOOP, 0x406aa7);
}

uint32_t game_op_call_offset_done(void)
{
	cpu.r[ESI] = pop32();
	X86_COUNT(1);
	return loop_at(0x406aa8);
}

/* 11: wait for a click (0x406acc), then clear the text window in the text
 * colour (game_op_wait_clear). */
uint32_t game_op_wait(void)
{
	return call_exit(WAIT_CLICK, 0x4065fb);
}

uint32_t game_op_wait_clear(void)
{
	cpu.r[EBX] = rd16(VM_TEXT_COLOUR);
	X86_COUNT(2);
	uint32_t esp = cpu.r[ESP];
	push32(X86_RET_SENTINEL);
	x86_exec(0x406b24); /* clear the text window: draws, no waiting */
	if (cpu.r[ESP] != esp)
		x86_trap(0x406b24, "text window clear did not return");
	return loop_at(0x406607);
}

/* 16 [n]: wait n/4 + 1 frames (0x4075c6, ECX = n), or with no argument for
 * a click (0x406acc). The code after either call is `jmp 0x4060f9`, left
 * native. */
uint32_t game_op_delay(void)
{
	game_parse_args_regs();
	X86_COUNT(2);
	if (cpu.r[ECX]) {
		cpu.r[ECX] = cpu.r[EAX];
		X86_COUNT(1);
		return call_exit(WAIT_FRAMES, 0x406abd);
	}
	return call_exit(WAIT_CLICK, 0x406ac7);
}

/* ---- calling into the native routines that load files and draw */

/* `call addr` into native code that returns (file loads, the anim step),
 * the CPU as it leaves it */
static void ncall(uint32_t addr)
{
	uint32_t esp = cpu.r[ESP];
	push32(X86_RET_SENTINEL);
	x86_exec(addr);
	if (cpu.r[ESP] != esp)
		x86_trap(addr, "native routine called by an opcode handler did not return");
}

static void load(uint32_t name, uint32_t dst)
{
	push32(dst);
	push32(name);
	X86_COUNT(2);
	ncall(0x401050);
	cpu.r[ESP] += 8;
	X86_COUNT(1);
}

/* rep movsd of n dwords from esi to edi (ESI, EDI end past them) */
static void movsd(uint32_t n, uint32_t esi, uint32_t edi)
{
	if (n > 0x10000)
		x86_trap(0x40705e, "rep movsd count with its upper half set (the native copy runs off memory)");
	for (uint32_t i = 0; i < n; i++)
		wr32(edi + 4 * i, rd32(esi + 4 * i));
	cpu.r[ESI] = esi + 4 * n;
	cpu.r[EDI] = edi + 4 * n;
	cpu.r[ECX] = 0;
	X86_COUNT(n);
}

#define MES_NAME 0x444e78u   /* name of the running MES file (32 bytes) */
#define CLICK_BUSY 0x445ce1u /* u8: a click handler block is running */
#define VM_TOP_ESP 0x445cdcu /* ESP of the top-level VM (0x4060d5) */
#define FLAG_NAME 0x445d86u  /* "FLAGn" (n at 0x445d8a) */
#define VM_FLAGS9 0x4452e0u  /* u8 @[9]: bits 5-6 survive a FLAG load */

/* 04 1e and the end of 04 21 0: load the MES file named at MES_NAME over
 * the script, reset the animation slots and the click handlers, and
 * restart the VM at the file's entry on the top-level stack. */
static uint32_t restart(void)
{
	load(MES_NAME, VM_SCRIPT);
	ncall(0x406dff);
	wr8(CLICK_BUSY, 0);
	wr16(VM_CLICK_HANDLERS, 0);
	wr16(VM_CLICK_HANDLERS + 2, 0);
	cpu.r[ESI] = rd16(VM_SCRIPT);
	uint32_t esp = cpu.r[ESP];
	cpu.r[ESP] = rd32(VM_TOP_ESP);
	X86_COUNT(6);
	/* from a nested VM the stack drops back: that leaves the handler at
	 * 0x406fc8 (`jmp 0x4060f9`, where the oracle stops, x86_stop_at) */
	if (cpu.r[ESP] > esp)
		return 0x406fc8;
	return loop_at(0x406fc8);
}

/* 04 1e name: jump to another MES file. */
uint32_t game_op_jump_mes(void)
{
	game_parse_args_regs();
	cpu.r[EDI] = MES_NAME;
	game_string_arg_regs();
	X86_COUNT(3);
	return restart();
}

/* 04 21 mode, n: the FLAGn file. mode 0 loads it over the VM state (from
 * MES_NAME on: name, flags, registers ... 0xc00 bytes) and restarts in the
 * MES file it names; 1 saves those 0xc00 bytes; anything else loads it but
 * keeps the running file's name. A load keeps bits 5-6 of @[9] and redoes
 * the VM setup (0x406395). */
static void flag_load(void)
{
	uint32_t keep = rd8(VM_FLAGS9) & 0x60;
	cpu.r[EAX] = (cpu.r[EAX] & ~0xffu) | keep;
	push32(cpu.r[EAX]);
	X86_COUNT(3);
	load(FLAG_NAME, MES_NAME);
	cpu.r[EAX] = pop32();
	uint32_t f = (rd8(VM_FLAGS9) & 0x9f) | (cpu.r[EAX] & 0xff);
	cpu.r[EAX] = (cpu.r[EAX] & 0xffff00ffu) | f << 8;
	wr8(VM_FLAGS9, f);
	X86_COUNT(5);
	game_vm_init_regs();
	X86_COUNT(1);
}

uint32_t game_op_flag_file(void)
{
	game_parse_args_regs();
	uint32_t mode = cpu.r[EAX];
	uint32_t bx = (cpu.r[EBX] & 0xffff0000u) | rd16(VM_ARG_VALUES + 2);
	bx = (bx & ~0xffu) | ((bx + 0x30) & 0xff);
	cpu.r[EBX] = bx;
	wr8(FLAG_NAME + 4, bx);
	X86_COUNT(5);
	if (mode == 0) {
		flag_load();
		return restart();
	}
	X86_COUNT(2);
	if (mode == 1) {
		push32(0xc00);
		push32(MES_NAME);
		push32(FLAG_NAME);
		X86_COUNT(3);
		ncall(0x401160);
		cpu.r[ESP] += 12;
		X86_COUNT(1);
		return loop_at(0x40719f);
	}
	uint32_t esi = cpu.r[ESI];
	movsd(8, MES_NAME, 0x445d66);
	flag_load();
	movsd(8, 0x445d66, MES_NAME);
	cpu.r[ESI] = esi;
	X86_COUNT(8);
	return loop_at(0x40714d);
}

/* 0x4069fd: format AX as decimal at 0x445ce6 (EDI), for op 15. DL = the
 * number of digits (0: as many as the value has, 1-4: that many low
 * digits, else 5); DH != 0: ASCII between 06 marks, else Big5 full-width
 * digits (0xa2af + d) and a NUL. The first digit counts how often its
 * place value fits, so a value too big for the digits gives a "digit"
 * above 9, as natively. Out: AX = 0, BX = the last digit's code, CX = 1,
 * EDI past the digits (DI only, for ASCII), DL/DH kept. */
void game_format_number_regs(void)
{
	static const uint16_t place[5] = { 10000, 1000, 100, 10, 1 };
	uint32_t edi = 0x445ce6, ax = cpu.r[EAX] & 0xffff, bx = cpu.r[EBX];
	uint32_t dl = cpu.r[EDX] & 0xff, dh = (cpu.r[EDX] >> 8) & 0xff;
	X86_COUNT(3);
	if (dh) {
		wr8(edi++, VM_STRING);
		X86_COUNT(2);
	}
	int first;
	if (dl == 0)
		first = ax >= 10000 ? 0 : ax >= 1000 ? 1 : ax >= 100 ? 2 : ax >= 10 ? 3 : 4;
	else
		first = dl == 1 ? 4 : dl == 2 ? 3 : dl == 3 ? 2 : dl == 4 ? 1 : 0;
	X86_COUNT(2 * (uint32_t)(5 - first) + 2);
	for (int i = first; i < 5; i++) {
		uint32_t cx = place[i];
		bx &= 0xffff0000u;
		X86_COUNT(3);
		while (ax >= cx) {
			ax -= cx;
			bx = (bx & 0xffff0000u) | ((bx + 1) & 0xffff);
			X86_COUNT(3);
			X86_POLL(0x406a70);
		}
		X86_COUNT(5);
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
		X86_COUNT(5);
	}
	cpu.r[EAX] = (cpu.r[EAX] & 0xffff0000u) | ax;
	cpu.r[EBX] = bx;
	cpu.r[ECX] = (cpu.r[ECX] & 0xffff0000u) | 1;
	cpu.r[EDI] = edi;
}

#define NUMBER_TEXT 0x445ce6u   /* 0x4069fd writes the digits here */
#define NUMBER_FORMAT 0x4452f7u /* u8: low nibble digits, high nibble ASCII */

/* 15 v: print v as text: format it (0x4069fd), then run the digits as a
 * script (a nested VM at NUMBER_TEXT, which draws them like any text)
 * and go on. */
uint32_t game_op_print_number(void)
{
	game_parse_args_regs();
	uint32_t f = rd8(NUMBER_FORMAT);
	cpu.r[EDX] = (cpu.r[EDX] & 0xffff0000u) | (f & 0xf0) << 8 | (f & 0x0f);
	X86_COUNT(5);
	game_format_number_regs();
	push32(cpu.r[ESI]);
	cpu.r[ESI] = NUMBER_TEXT - VM_SCRIPT;
	X86_COUNT(4);
	return call_exit(VM_LOOP, 0x4069f7);
}

uint32_t game_op_print_number_done(void)
{
	cpu.r[ESI] = pop32();
	X86_COUNT(1);
	return loop_at(0x4069f8);
}

/* 04 1f name: run another MES file as a subroutine. The labels (0x1e
 * dwords from VM_LABELS) and the file name are saved, the file is loaded
 * over the script, the click handler state is pushed and cleared, and the
 * file runs as a nested VM from its entry (game_op_call_mes_done goes on
 * when it returns). */
#define SAVED_LABELS 0x445e36u
#define SAVED_NAME 0x445d46u

uint32_t game_op_call_mes(void)
{
	game_parse_args_regs();
	push32(cpu.r[ESI]);
	movsd(0x1e, VM_LABELS, SAVED_LABELS);
	movsd(8, MES_NAME, SAVED_NAME);
	cpu.r[EDI] = MES_NAME;
	game_string_arg_regs();
	X86_COUNT(8);
	load(MES_NAME, VM_SCRIPT);
	cpu.r[EAX] = (cpu.r[EAX] & ~0xffu) | rd8(CLICK_BUSY);
	push32(cpu.r[EAX]);
	cpu.r[EAX] = (cpu.r[EAX] & ~0xffffu) | rd16(VM_CLICK_HANDLERS);
	push32(cpu.r[EAX]);
	cpu.r[EAX] = (cpu.r[EAX] & ~0xffffu) | rd16(VM_CLICK_HANDLERS + 2);
	push32(cpu.r[EAX]);
	wr8(CLICK_BUSY, 0);
	wr16(VM_CLICK_HANDLERS, 0);
	wr16(VM_CLICK_HANDLERS + 2, 0);
	cpu.r[ESI] = rd16(VM_SCRIPT);
	X86_COUNT(10);
	return call_exit(VM_LOOP, 0x40704a);
}

/* 04 1f after the file returned: restore the click handlers, the labels
 * and the name (`mov cx, n`: the count takes ECX's upper half as the
 * nested VM left it, zero from the message pump), reload the caller's
 * file and go on. */
uint32_t game_op_call_mes_done(void)
{
	cpu.r[EAX] = pop32();
	wr16(VM_CLICK_HANDLERS + 2, cpu.r[EAX]);
	cpu.r[EAX] = pop32();
	wr16(VM_CLICK_HANDLERS, cpu.r[EAX]);
	cpu.r[EAX] = pop32();
	wr8(CLICK_BUSY, cpu.r[EAX]);
	X86_COUNT(6);
	movsd((cpu.r[ECX] & 0xffff0000u) | 0x1e, SAVED_LABELS, VM_LABELS);
	movsd(8, SAVED_NAME, MES_NAME);
	X86_COUNT(6);
	load(MES_NAME, VM_SCRIPT);
	cpu.r[ESI] = pop32();
	X86_COUNT(1);
	return loop_at(0x407091);
}

/* ---- 04 13 [n]: the menu / event loop (0x406671)
 *
 * The block at label n (or the inline block given as the argument) is the
 * menu (0x445e0c, for 04 14); label 38 runs first, then each round: pump
 * (0x401bf0; WM_QUIT ends the VM), step the animations (0x403624), and
 * while [0x445304] stays set, run label 39, then for each input k = 0..5
 * enabled in 0x4452e1 (bit 1 << (k + 1): click, right click, the arrows)
 * that is down (0x4074d5 k), run label 32 + k and, unless bit 0 of
 * 0x4452e1 is set, wait until k is released (stepping the animations
 * meanwhile for k = 5). The pieces after each call are jump units; all of
 * them run with the statement's ESI pushed. */
#define MENU_BLOCK 0x445e0cu /* u16: the menu's block */
#define MENU_RUN 0x445304u   /* u16: the loop runs while set (scripts clear it) */
#define MENU_INPUTS 0x4452e1u
#define MENU_INIT 0x445e08u  /* label 38 */
#define MENU_FRAME 0x445e0au /* label 39 */
#define PUMP 0x401bf0u
#define INPUT_TEST 0x4074d5u
#define ANIM_STEP 0x403624u

static const struct menu_input {
	uint32_t bit, handler, ra_test, ra_block, ra_wait;
	uint32_t pc_pump_after_block, pc_wait_loop, pc_pump_after_wait;
} menu_inputs[6] = {
	{ 0x02, 0x445dfc, 0x4066f6, 0x406704, 0x406714, 0x40670b, 0x406714, 0x406716 },
	{ 0x04, 0x445dfe, 0x406728, 0x406736, 0x40674a, 0x40673d, 0x40674a, 0x40674c },
	{ 0x08, 0x445e00, 0x406761, 0x40676f, 0x406783, 0x406776, 0x406783, 0x406785 },
	{ 0x10, 0x445e02, 0x40679a, 0x4067a8, 0x4067bc, 0x4067af, 0x4067bc, 0x4067be },
	{ 0x20, 0x445e04, 0x4067d3, 0x4067e1, 0x4067f5, 0x4067e8, 0x4067f5, 0x4067f7 },
	{ 0x40, 0x445e06, 0x406810, 0x406822, 0x40683b, 0x406829, 0x40683b, 0x40683d },
};

uint32_t game_op_menu(void)
{
	game_parse_args_regs();
	uint32_t type = rd8(VM_ARG_TYPES);
	cpu.r[EAX] = (cpu.r[EAX] & ~0xffu) | type;
	X86_COUNT(3);
	if (type == VM_ARG_BLOCK) {
		cpu.r[EAX] = (cpu.r[EAX] & ~0xffffu) | rd16(VM_ARG_VALUES);
	} else {
		cpu.r[EAX] = rd16(VM_ARG_VALUES);
		cpu.r[EAX] = rd16(VM_LABELS + 2 * cpu.r[EAX]);
		X86_COUNT(2);
	}
	wr16(MENU_BLOCK, cpu.r[EAX]);
	wr16(MENU_RUN, 1);
	push32(cpu.r[ESI]);
	cpu.r[ESI] = rd16(MENU_INIT);
	X86_COUNT(5);
	return call_exit(RUN_BLOCK, 0x4066b8);
}

/* the round's start (0x4066b8; the pieces below jump there from
 * native_pc, backward, so they poll) */
static uint32_t menu_pump(uint32_t native_pc)
{
	if (native_pc)
		X86_POLL(native_pc);
	return call_exit(PUMP, 0x4066bd);
}

/* from input k on: test the first one enabled, or start the next round */
static uint32_t menu_scan(int k)
{
	for (; k < 6; k++) {
		X86_COUNT(2);
		if (rd8(MENU_INPUTS) & menu_inputs[k].bit) {
			cpu.r[EAX] = (cpu.r[EAX] & ~0xffu) | (uint32_t)k;
			X86_COUNT(1);
			return call_exit(INPUT_TEST, menu_inputs[k].ra_test);
		}
	}
	return menu_pump(0x406803);
}

static uint32_t menu_wait(int k)
{
	if (k == 5)
		ncall(ANIM_STEP);
	cpu.r[EAX] = (cpu.r[EAX] & ~0xffu) | (uint32_t)k;
	X86_COUNT(1);
	return call_exit(INPUT_TEST, menu_inputs[k].ra_wait);
}

uint32_t game_op_menu_round(void) /* 0x4066b8: after label 38 */
{
	return menu_pump(0);
}

uint32_t game_op_menu_pumped(void) /* 0x4066bd */
{
	X86_COUNT(2);
	if (!cpu.r[EAX]) { /* WM_QUIT: 0x40638d, mov esp, [0x445cdc] */
		cpu.r[ESP] = rd32(VM_TOP_ESP);
		X86_COUNT(1);
		return 0x406393;
	}
	ncall(ANIM_STEP);
	X86_COUNT(2);
	if (!rd16(MENU_RUN)) {
		cpu.r[ESI] = pop32();
		X86_COUNT(1);
		return loop_at(0x4066d5);
	}
	cpu.r[ESI] = rd16(MENU_FRAME);
	X86_COUNT(1);
	return call_exit(RUN_BLOCK, 0x4066e6);
}

uint32_t game_op_menu_framed(void) /* 0x4066e6: after label 39 */
{
	return menu_scan(0);
}

/* after 0x4074d5 k (ZF set: k is not down) */
static uint32_t menu_tested(int k)
{
	X86_COUNT(1);
	if (cpu.zf)
		return k == 5 ? menu_pump(0x406810) : menu_scan(k + 1);
	cpu.r[ESI] = rd16(menu_inputs[k].handler);
	X86_COUNT(1);
	return call_exit(RUN_BLOCK, menu_inputs[k].ra_block);
}

/* after label 32 + k */
static uint32_t menu_handled(int k)
{
	X86_COUNT(2);
	if (rd8(MENU_INPUTS) & 1)
		return menu_pump(menu_inputs[k].pc_pump_after_block);
	return menu_wait(k);
}

/* after 0x4074d5 k while waiting for the release (ZF clear: still down) */
static uint32_t menu_waited(int k)
{
	X86_COUNT(1);
	if (!cpu.zf) {
		X86_POLL(menu_inputs[k].pc_wait_loop);
		return menu_wait(k);
	}
	return menu_pump(menu_inputs[k].pc_pump_after_wait);
}

#define MENU_INPUT_PIECES(k) \
	uint32_t game_op_menu_tested##k(void) { return menu_tested(k); } \
	uint32_t game_op_menu_handled##k(void) { return menu_handled(k); } \
	uint32_t game_op_menu_waited##k(void) { return menu_waited(k); }
MENU_INPUT_PIECES(0)
MENU_INPUT_PIECES(1)
MENU_INPUT_PIECES(2)
MENU_INPUT_PIECES(3)
MENU_INPUT_PIECES(4)
MENU_INPUT_PIECES(5)
