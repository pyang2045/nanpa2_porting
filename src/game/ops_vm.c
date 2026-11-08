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
