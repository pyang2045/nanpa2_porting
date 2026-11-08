/*
 * Script expressions and flags: the operand language of the MES VM.
 *
 * Reimplements the native expression evaluator 0x4037fc (with its operator
 * handlers 0x4038ae-0x4039d1 and flag read 0x4039d6), the constant reader
 * 0x4039f7 (tails 0x403a06, 0x403a12, 0x403a22, 0x403a42) and the flag
 * write 0x406455. Semantics: re-notes.md "Script expressions".
 */
#include "game/expr.h"

#include <stdio.h>

#include "game/vm.h"
#include "x86/x86.h"

enum {
	TOK_END = 0x03,       /* end of expression */
	TOK_BYTE = 0x07,      /* 07 b: b >> 2 */
	TOK_WORD12 = 0x08,    /* 08 h l: 12 bits */
	TOK_WORD18 = 0x09,    /* 09 h m l: 18 bits, kept to 16 */
	TOK_DIGIT = 0x30,     /* 0x30-0x3f: 0-15 */
	TOK_REG = 0x40,       /* 0x40-0x5a: registers '@'-'Z' */
	TOK_OP = 0x20,        /* 0x20-0x2f: operators */
};

enum {
	OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_OR, OP_AND, OP_EQ, OP_NE,
	OP_ABOVE, OP_BELOW, OP_WORD_AT, OP_BYTE_AT, OP_FLAG, OP_FLAG_AT, OP_RANDOM,
};

/* Each script byte of a constant carries 6 bits, in its upper bits. */
static uint32_t six(uint32_t b)
{
	return (b & 0xff) >> 2;
}

int script_read_constant(uint32_t *pc, uint32_t *value)
{
	uint32_t t = vm_script_byte((*pc)++);
	if (t >= TOK_DIGIT && t < TOK_DIGIT + 16) {
		*value = t - TOK_DIGIT;
		return 1;
	}
	if (t == TOK_BYTE) {
		*value = six(vm_script_byte((*pc)++));
		return 1;
	}
	if (t == TOK_WORD12) {
		uint32_t h = vm_script_byte(*pc), l = vm_script_byte(*pc + 1);
		*pc += 2;
		*value = six(h) << 6 | six(l);
		return 1;
	}
	if (t == TOK_WORD18) {
		uint32_t h = vm_script_byte(*pc), m = vm_script_byte(*pc + 1), l = vm_script_byte(*pc + 2);
		*pc += 3;
		*value = (six(h) << 12 | six(m) << 6 | six(l)) & 0xffff;
		return 1;
	}
	*value = t;
	return 0;
}

uint32_t script_flag(uint32_t n)
{
	uint32_t b = rd8(VM_FLAGS + (n >> 1));
	return n & 1 ? b >> 4 : b & 0xf;
}

void script_set_flag(uint32_t n, uint32_t v)
{
	uint32_t a = VM_FLAGS + (n >> 1);
	v &= 0xffff;
	if (v > 15)
		v = 15;
	uint32_t b = rd8(a);
	wr8(a, n & 1 ? (b & 0x0f) | v << 4 : (b & 0xf0) | v);
}

/* The 16-bit generator of 0x403999: seed = 5 * seed + 1. */
static uint32_t next_random(uint32_t range)
{
	uint32_t seed = (rd16(VM_RANDOM_SEED) * 5 + 1) & 0xffff;
	wr16(VM_RANDOM_SEED, seed);
	range &= 0xffff;
	if (!range)
		x86_divide_error(); /* div bx */
	return seed % range;
}

/* Element of the word array whose segment-0 offset is base: the array
 * through register '@' lives in segment 0 (it is VM_ARRAY), every other
 * base is in the segment that @[7] selects. Like the native code (16-bit
 * load into AX after adding the doubled index in EAX) the result keeps the
 * upper half of 2 * index. */
static uint32_t word_at(uint32_t base, uint32_t index)
{
	uint32_t seg = (base & 0xffff) == vm_reg(0) ? 0 : vm_segment_select();
	uint32_t off = 2 * index;
	return (off & 0xffff0000u) | rd16(vm_segment_base(seg) + base + off);
}

static uint32_t byte_at(uint32_t base, uint32_t index)
{
	return rd8(vm_segment_base(vm_segment_select()) + base + index);
}

uint64_t script_eval_coverage[256];

#define STACK_MAX 64

static _Noreturn void bad_expression(uint32_t pc, const char *why)
{
	char msg[96];
	snprintf(msg, sizeof msg, "script expression at %04x: %s", pc, why);
	x86_trap(0x4037fc, msg);
}

/* Instructions the native loop (0x4037fc) runs for token t, counted on its
 * paths: the compare chain from 0x403803 and the token's own code (0x403a06
 * digits, 0x403a12 07, 0x403a22 08, 0x403a42 09, 0x40383d registers,
 * 0x403866 the operator table, 0x4038ad a bare token). */
static uint32_t token_insns(uint32_t t)
{
	if (t == TOK_END)
		return 6;
	if (t >= TOK_DIGIT && t < TOK_DIGIT + 16)
		return 14;
	if (t == TOK_BYTE)
		return 15;
	if (t == TOK_WORD12)
		return 20;
	if (t == TOK_WORD18)
		return 29;
	if (t >= TOK_REG && t < TOK_REG + VM_REG_COUNT)
		return 23;
	if (t >= TOK_OP && t < TOK_OP + 16) {
		switch (t - TOK_OP) { /* 21 to reach the operator's code */
		case OP_FLAG: return 40;
		case OP_RANDOM: return 42;
		case OP_FLAG_AT: return 31;
		case OP_WORD_AT: return 33;
		case OP_BYTE_AT: return 29;
		default: return 27;
		}
	}
	return 17;
}

uint32_t script_eval(uint32_t *pc, int *bare_token, uint32_t *insns)
{
	uint32_t stack[STACK_MAX];
	int depth = 0;
	uint32_t start = *pc;
	*bare_token = -1;
	*insns = 0;
	for (;;) {
		uint32_t t = vm_script_byte(*pc), v;
		*insns += token_insns(t);
		script_eval_coverage[t]++;
		if (t == TOK_END) {
			++*pc;
			if (depth != 1)
				bad_expression(start, depth ? "values left over" : "no value");
			return stack[0];
		}
		if (t >= TOK_OP && t < TOK_OP + 16) {
			++*pc;
			uint32_t op = t - TOK_OP, a = 0, b = 0;
			/* unary: the flag and random operators. A byte that is
			 * not a constant serves as the number itself (0x4039f7
			 * falls through with EAX = that byte; 0x403861 cleared
			 * the upper bytes). */
			if (op == OP_FLAG) {
				script_read_constant(pc, &v);
				v = script_flag(v);
			} else if (op == OP_RANDOM) {
				script_read_constant(pc, &v);
				v = next_random(v);
			} else if (op == OP_FLAG_AT) {
				if (depth < 1)
					bad_expression(start, "stack underflow");
				v = script_flag(stack[--depth]);
			} else {
				if (depth < 2)
					bad_expression(start, "stack underflow");
				b = stack[--depth]; /* top */
				a = stack[--depth];
				switch (op) {
				case OP_ADD: v = a + b; break;
				case OP_SUB: v = a >= b ? a - b : 0; break;
				case OP_MUL: v = a * b; break;
				case OP_DIV:
				case OP_MOD:
					if (!b)
						x86_divide_error();
					v = op == OP_DIV ? a / b : a % b;
					break;
				case OP_OR: v = a | b; break;
				case OP_AND: v = a & b; break;
				case OP_EQ: v = a == b; break;
				case OP_NE: v = a != b; break;
				case OP_ABOVE: v = a > b; break;
				case OP_BELOW: v = a < b; break;
				case OP_WORD_AT: v = word_at(a, b); break;
				default: v = byte_at(a, b); break; /* OP_BYTE_AT */
				}
			}
		} else if (t >= TOK_REG && t < TOK_REG + VM_REG_COUNT) {
			++*pc;
			v = vm_reg(t - TOK_REG);
		} else if (!script_read_constant(pc, &v)) {
			/* not an operand: the native code returns at once (0x4038ad),
			 * which only works while nothing is on its stack */
			if (depth)
				bad_expression(start, "unknown token");
			*bare_token = (int)t;
			return t;
		}
		if (depth == STACK_MAX)
			bad_expression(start, "stack overflow");
		stack[depth++] = v;
	}
}

/* ---- native calling conventions ---- */

/* 0x4037fc: in ESI = script offset; out EAX = value, ESI past the 0x03.
 * EBX and EDX are scratch: every call site (0x406499, 0x4064b5, 0x4064bd,
 * 0x4064e5, 0x406522, 0x40652d, 0x40656b, 0x406574, 0x40659e, 0x40660e,
 * 0x4068f9, 0x40747c) saves EBX around the call or overwrites it, and
 * reloads EDX before using it; none reads the flags. ECX, EDI and EBP are
 * preserved. */
void game_eval_regs(void)
{
	uint32_t pc = cpu.r[ESI], insns;
	int bare;
	uint32_t v = script_eval(&pc, &bare, &insns);
	cpu.r[ESI] = pc;
	cpu.r[EAX] = bare >= 0 ? (cpu.r[EAX] & 0xffffff00u) | v : v;
	X86_COUNT(insns); /* emulated CPU time, as the native loop runs */
}

/* 0x4039f7: in ESI; out EAX = value, ESI past it. A non-constant byte
 * lands in AL with EAX's upper bytes kept; the 18-bit form leaves the value
 * in BX (its only caller, 0x406491, overwrites EBX). */
void game_read_constant_regs(void)
{
	uint32_t pc = cpu.r[ESI], v;
	uint32_t t = vm_script_byte(pc);
	if (script_read_constant(&pc, &v)) {
		cpu.r[EAX] = v;
		if (t == TOK_WORD18)
			cpu.r[EBX] = (cpu.r[EBX] & 0xffff0000u) | v;
	} else {
		cpu.r[EAX] = (cpu.r[EAX] & 0xffffff00u) | v;
	}
	cpu.r[ESI] = pc;
	X86_COUNT(8);
}

/* 0x406455: in EBX = flag number, AX = value. EBX preserved; AX is left
 * clamped to 15 and, for an odd flag, AL shifted into the high nibble. */
void game_set_flag_regs(void)
{
	uint32_t n = cpu.r[EBX], ax = cpu.r[EAX] & 0xffff;
	script_set_flag(n, ax);
	if (ax > 15)
		ax = 15;
	if (n & 1)
		ax = (ax & 0xff00) | ((ax << 4) & 0xf0);
	cpu.r[EAX] = (cpu.r[EAX] & 0xffff0000u) | ax;
	X86_COUNT(9);
}
