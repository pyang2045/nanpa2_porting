/*
 * x86 interpreter: decoded instructions (interp.c). The decoder reads the
 * game code from emulated memory, where x86_load_image put the player's
 * nanpa2.exe; nothing of the exe is compiled in. Decoded instructions are
 * cached per code address (the game code never modifies itself).
 */
#ifndef NANPA2_INTERP_H
#define NANPA2_INTERP_H

#include <stdint.h>

#include "x86.h"

#define CODE_LEN (CODE_END - CODE_START)
#define NOREG 8

struct dec {
	uint16_t op;    /* opcode byte, 0x100 + second byte for 0f xx */
	uint8_t len;    /* 0 = not decoded yet */
	uint8_t osz;    /* operand size of word/dword forms: 2 or 4 */
	uint8_t rep;    /* 0, 0xf3 (rep/repe) or 0xf2 (repne) */
	uint8_t modreg; /* 1: r/m is a register (mod 3) */
	uint8_t reg;    /* ModRM reg field (or the register of 40-5f, 90-bf) */
	uint8_t rm;     /* ModRM r/m register when modreg */
	uint8_t base, index, scale; /* memory operand (NOREG = none) */
	uint8_t probe;  /* x86_probe before this instruction */
	int32_t disp;
	uint32_t imm;   /* immediate (sign-extended where x86 does), or the
			 * absolute target of a relative branch */
	uint32_t imm2;  /* second immediate: shld/shrd count */
};

/* per code byte: 1 = an override (HLE_OVERRIDE): entering it calls hle_call */
extern uint8_t x86_interp_override[CODE_LEN];
/* decoded instruction per code byte (len 0 = not decoded yet) */
extern struct dec dec_cache[CODE_LEN];
const struct dec *x86_decode(uint32_t pc);
void x86_interp_init(void);
_Noreturn void x86_unimplemented(uint32_t pc);
/* hle/hle.h, not included here to keep the x86 layer apart */
void hle_call(uint32_t addr);

/* Entering pc leaves the game code: an import or COM thunk, the C runtime
 * or an override, all run by hle_call. */
static inline int x86_is_hle(uint32_t pc)
{
	uint32_t off = pc - CODE_START;
	return off >= CODE_LEN || x86_interp_override[off];
}

static inline uint32_t reg_get(int r, int sz)
{
	if (sz == 4)
		return cpu.r[r];
	if (sz == 2)
		return cpu.r[r] & 0xffffu;
	return r < 4 ? cpu.r[r] & 0xffu : (cpu.r[r - 4] >> 8) & 0xffu;
}

static inline void reg_put(int r, int sz, uint32_t v)
{
	if (sz == 4)
		cpu.r[r] = v;
	else if (sz == 2)
		set16(r, v);
	else if (r < 4)
		set8l(r, v);
	else
		set8h(r - 4, v);
}

static inline uint32_t dec_ea(const struct dec *d)
{
	uint32_t a = (uint32_t)d->disp;
	if (d->base != NOREG)
		a += cpu.r[d->base];
	if (d->index != NOREG)
		a += cpu.r[d->index] << d->scale;
	return a;
}

/* condition codes of jcc/setcc, low nibble of the opcode */
static inline int cond(uint32_t c)
{
	switch (c) {
	case 0x0: return cpu.of != 0;
	case 0x1: return !cpu.of;
	case 0x2: return cpu.cf != 0;
	case 0x3: return !cpu.cf;
	case 0x4: return cpu.zf != 0;
	case 0x5: return !cpu.zf;
	case 0x6: return cpu.cf || cpu.zf;
	case 0x7: return !cpu.cf && !cpu.zf;
	case 0x8: return cpu.sf != 0;
	case 0x9: return !cpu.sf;
	case 0xa: return cpu.pf != 0;
	case 0xb: return !cpu.pf;
	case 0xc: return cpu.sf != cpu.of;
	case 0xd: return cpu.sf == cpu.of;
	case 0xe: return cpu.zf || cpu.sf != cpu.of;
	default: return !cpu.zf && cpu.sf == cpu.of;
	}
}

/* group 1: add or adc sbb and sub xor cmp (k = 0..7) */
static inline uint32_t alu(int k, uint32_t a, uint32_t b, int bits)
{
	switch (k) {
	case 0: return f_add(a, b, 0, bits);
	case 1: return f_logic(a | b, bits);
	case 2: return f_add(a, b, cpu.cf, bits);
	case 3: return f_sub(a, b, cpu.cf, bits);
	case 4: return f_logic(a & b, bits);
	case 6: return f_logic(a ^ b, bits);
	default: return f_sub(a, b, 0, bits); /* sub, cmp */
	}
}

#endif
