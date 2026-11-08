/*
 * Script VM helpers: block skip, argument lists, string arguments, VM
 * setup, menu items (re-notes.md "Script VM helpers").
 *
 * Reimplements 0x4061d4 (block skip), 0x40741b with 0x4074a3 (argument
 * parser), 0x406dc4 (string argument), 0x406395 (VM init) and 0x4068e1
 * (menu item step). The register-level wrappers at the end give each its
 * native contract; the C functions above them are what the opcode
 * handlers in src/game/ops.c call.
 */
#include <stdio.h>

#include "game/expr.h"
#include "game/vm.h"
#include "x86/x86.h"

uint32_t vm_eval(uint32_t *pc)
{
	uint32_t start = *pc, insns;
	int bare;
	uint32_t v = script_eval(pc, &bare, &insns);
	X86_COUNT(insns);
	if (bare >= 0) {
		/* natively the value would be the token in AL over whatever EAX
		 * held; no script needs that */
		char msg[80];
		snprintf(msg, sizeof msg, "script at %04x: token %02x where a value belongs", start, bare);
		x86_trap(0x4037fc, msg);
	}
	return v;
}

uint32_t vm_skip_block(uint32_t pc)
{
	uint32_t start = pc;
	while (vm_script_byte(pc) != VM_BLOCK_OPEN)
		pc++;
	pc++;
	uint32_t body = pc;
	/* the native depth counter is AH: 8 bits, wrapping */
	uint8_t depth = 1;
	for (;;) {
		uint32_t b = vm_script_byte(pc++);
		if (b == VM_BLOCK_OPEN)
			depth++;
		else if (b == VM_BLOCK_END && --depth == 0)
			break;
	}
	/* 4 instructions a byte up to the 01, about 6 a byte in the block */
	X86_COUNT(4 + 4 * (body - start) + 6 * (pc - body));
	return pc;
}

/* 0x4074a3: tokens that can start an expression: 07 08 09 constants, then
 * 0x20-0x5a (operators, digits, registers). */
static int starts_expression(uint32_t b)
{
	return b == 0x07 || b == 0x08 || b == 0x09 || (b >= 0x20 && b < 0x5b);
}

uint32_t vm_parse_args(uint32_t *pc, uint32_t *eax)
{
	uint32_t p = *pc, n = 0, a = 0;
	for (uint32_t i = 0; i < VM_ARG_TYPES_CLEARED; i += 4)
		wr32(VM_ARG_TYPES + i, 0);
	X86_COUNT(14);
	for (;;) {
		uint32_t b = vm_script_byte(p);
		/* type and value go to entry n with no bound check: past 16
		 * arguments the types run into the values, as natively */
		if (b == VM_STRING) {
			wr8(VM_ARG_TYPES + n, VM_ARG_STRING);
			wr16(VM_ARG_VALUES + 2 * n, p);
			n++;
			uint32_t s = ++p;
			do
				a = (a & ~0xffu) | vm_script_byte(p++);
			while ((a & 0xff) != VM_STRING);
			X86_COUNT(8 + 4 * (p - s));
		} else if (b == VM_BLOCK_OPEN) {
			wr8(VM_ARG_TYPES + n, VM_ARG_BLOCK);
			wr16(VM_ARG_VALUES + 2 * n, p);
			n++;
			p = vm_skip_block(p);
			a &= ~0xffffu; /* 0x4061d4 leaves AX = 0 */
			X86_COUNT(8);
		} else if (!starts_expression(b)) {
			a = (a & ~0xffu) | 0xff; /* AL = -1 from 0x4074a3 */
			X86_COUNT(14);
			break;
		} else {
			a = vm_eval(&p);
			wr8(VM_ARG_TYPES + n, VM_ARG_EXPR);
			wr16(VM_ARG_VALUES + 2 * n, a);
			n++;
			X86_COUNT(26);
		}
		if (vm_script_byte(p) != VM_SEPARATOR) {
			a = vm_arg(0);
			X86_COUNT(4);
			break;
		}
		p++;
		X86_COUNT(4);
	}
	*pc = p;
	*eax = a;
	return n;
}

/* ---- native calling conventions ---- */

/* 0x4061d4: ESI in/out. The native loop leaves AL = 0 (the closing byte)
 * and AH = 0 (the depth); the flags are dead at all six call sites
 * (tools/liveness.py). */
void game_skip_block_regs(void)
{
	cpu.r[ESI] = vm_skip_block(cpu.r[ESI]);
	set16(EAX, 0);
}

/* 0x406395: set up '@', the segment table and @[0], @[22] before a MES
 * file runs. EAX is scratch (dead after its three call sites). */
void game_vm_init_regs(void)
{
	vm_set_reg(0, VM_ARRAY - VM_SCRIPT);
	wr32(VM_SEGMENTS + 0, VM_SCRIPT);
	wr32(VM_SEGMENTS + 4, VM_SEGMENT1);
	wr32(VM_SEGMENTS + 8, VM_SEGMENT2);
	wr32(VM_SEGMENTS + 12, VM_SEGMENT2);
	wr16(VM_ARRAY, VM_ARRAY0_BUFFER - VM_SCRIPT);
	wr16(VM_MENU_LIST_PTR, VM_MENU_LIST - VM_SCRIPT);
	X86_COUNT(17);
}

/* 0x4068e1: one item of a menu list ("02 [0f expr 03] text", ended by 00).
 * In ESI at the item; out AL = -1 at the end of the list (nothing else
 * changes), else AL = 1, or the guard's value when there is a 0f guard
 * (EAX all of it); BX = the offset of the item's text and ESI the next 00
 * or 02 after it. EDX is the evaluator's scratch; the flags are dead after
 * both call sites. */
void game_menu_item_regs(void)
{
	uint32_t pc = cpu.r[ESI], a = cpu.r[EAX];
	if (vm_script_byte(pc) == VM_BLOCK_END) {
		cpu.r[EAX] = (a & ~0xffu) | 0xff;
		X86_COUNT(4);
		return;
	}
	pc++; /* the 02 (any byte, in fact) */
	a = (a & ~0xffu) | 1;
	if (vm_script_byte(pc) == VM_IF) {
		pc++;
		uint32_t insns;
		int bare;
		uint32_t v = script_eval(&pc, &bare, &insns);
		X86_COUNT(insns);
		a = bare >= 0 ? (a & ~0xffu) | v : v;
	}
	cpu.r[EBX] = pc;
	uint32_t start = pc;
	while (vm_script_byte(pc) != VM_BLOCK_END && vm_script_byte(pc) != VM_SEPARATOR)
		pc++;
	cpu.r[ESI] = pc;
	cpu.r[EAX] = a;
	X86_COUNT(10 + 6 * (pc - start));
}

/* 0x406dc4: copy argument 0 as a string to EDI. A string literal (type 2,
 * the value is the offset of its opening 06) is copied up to its closing
 * 06 and NUL-terminated, EDI ending on the NUL and AL = 06; an expression
 * (type 1, the value is the script offset of a NUL-terminated string) is
 * copied with its NUL, EDI ending past it and AL = 0. Either way EBX is the
 * script offset just past the last byte read. EDI is live after (the
 * callers' ECX passes through untouched). */
void game_string_arg_regs(void)
{
	uint32_t dst = cpu.r[EDI], src = vm_arg(0), b;
	uint32_t start = dst;
	if (rd8(VM_ARG_TYPES) == VM_ARG_EXPR) {
		do {
			b = vm_script_byte(src++);
			wr8(dst++, b);
		} while (b);
	} else {
		src++;
		while ((b = vm_script_byte(src++)) != VM_STRING)
			wr8(dst++, b);
		wr8(dst, 0);
	}
	cpu.r[EBX] = src;
	cpu.r[EDI] = dst;
	cpu.r[EAX] = (cpu.r[EAX] & ~0xffu) | b;
	X86_COUNT(6 + 6 * (dst - start));
}

/* 0x40741b: ESI in/out; out EAX (argument 0's value, or AL = -1 over the
 * last value when the list ends at a byte that starts no argument), ECX =
 * the count, EBX = VM_ARG_TYPES + count, EDI = VM_ARG_VALUES + 2 * count
 * (some callers read each). EDX is the evaluator's scratch. */
void game_parse_args_regs(void)
{
	uint32_t pc = cpu.r[ESI], a;
	uint32_t n = vm_parse_args(&pc, &a);
	cpu.r[ESI] = pc;
	cpu.r[EAX] = a;
	cpu.r[ECX] = n;
	cpu.r[EBX] = VM_ARG_TYPES + n;
	cpu.r[EDI] = VM_ARG_VALUES + 2 * n;
}

void game_ncall(uint32_t addr, uint32_t popped)
{
	uint32_t esp = cpu.r[ESP] + 4 * popped;
	push32(X86_RET_SENTINEL);
	x86_exec(addr);
	if (cpu.r[ESP] != esp)
		x86_trap(addr, "native routine called by reimplemented code did not return");
}
