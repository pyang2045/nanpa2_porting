/*
 * Runtime for the code lifted from nanpa2.exe by tools/recomp.py.
 *
 * Emulated state: eight 32-bit registers, the arithmetic flags, and a flat
 * little-endian memory holding the exe image at its native addresses
 * (0x400000...), the native stack and a heap. Lifted code reads and writes
 * memory only through rd*()/wr*().
 */
#ifndef NANPA2_X86_H
#define NANPA2_X86_H

#include <stdint.h>
#include <string.h>

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

struct cpu {
	uint32_t r[8];
	uint32_t cf, zf, sf, of, pf, df;
};
extern struct cpu cpu;
extern uint8_t *x86_mem;

#define MEM_BASE 0x00400000u
#define MEM_END 0x04000000u
#define STACK_TOP 0x01000000u
#define HEAP_BASE 0x01000000u
/* Return address the host pushes before entering lifted code. */
#define X86_RET_SENTINEL 0xfffffff0u

_Noreturn void x86_trap(uint32_t pc, const char *why);
_Noreturn void x86_fault(uint32_t addr);

static inline uint8_t *x86_ptr(uint32_t a, uint32_t n)
{
	if (a < MEM_BASE || a + n > MEM_END || a + n < a)
		x86_fault(a);
	return x86_mem + (a - MEM_BASE);
}

static inline uint32_t rd8(uint32_t a) { return *x86_ptr(a, 1); }
static inline uint32_t rd16(uint32_t a) { uint16_t v; memcpy(&v, x86_ptr(a, 2), 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, x86_ptr(a, 4), 4); return v; }
static inline void wr8(uint32_t a, uint32_t v) { *x86_ptr(a, 1) = (uint8_t)v; }
static inline void wr16(uint32_t a, uint32_t v) { uint16_t t = (uint16_t)v; memcpy(x86_ptr(a, 2), &t, 2); }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(x86_ptr(a, 4), &v, 4); }

static inline void set16(int r, uint32_t v) { cpu.r[r] = (cpu.r[r] & 0xffff0000u) | (v & 0xffffu); }
static inline void set8l(int r, uint32_t v) { cpu.r[r] = (cpu.r[r] & 0xffffff00u) | (v & 0xffu); }
static inline void set8h(int r, uint32_t v) { cpu.r[r] = (cpu.r[r] & 0xffff00ffu) | ((v & 0xffu) << 8); }

static inline void push32(uint32_t v) { cpu.r[ESP] -= 4; wr32(cpu.r[ESP], v); }
static inline void push16(uint32_t v) { cpu.r[ESP] -= 2; wr16(cpu.r[ESP], v); }
static inline uint32_t pop32(void) { uint32_t v = rd32(cpu.r[ESP]); cpu.r[ESP] += 4; return v; }
static inline uint32_t pop16(void) { uint32_t v = rd16(cpu.r[ESP]); cpu.r[ESP] += 2; return v; }

static inline void pushal(void)
{
	uint32_t sp = cpu.r[ESP];
	push32(cpu.r[EAX]); push32(cpu.r[ECX]); push32(cpu.r[EDX]); push32(cpu.r[EBX]);
	push32(sp); push32(cpu.r[EBP]); push32(cpu.r[ESI]); push32(cpu.r[EDI]);
}

static inline void popal(void)
{
	cpu.r[EDI] = pop32(); cpu.r[ESI] = pop32(); cpu.r[EBP] = pop32(); cpu.r[ESP] += 4;
	cpu.r[EBX] = pop32(); cpu.r[EDX] = pop32(); cpu.r[ECX] = pop32(); cpu.r[EAX] = pop32();
}

static inline uint32_t get_eflags(void)
{
	return cpu.cf | 2u | (cpu.pf << 2) | (cpu.zf << 6) | (cpu.sf << 7) | (cpu.df << 10) | (cpu.of << 11);
}

static inline void set_eflags(uint32_t f)
{
	cpu.cf = f & 1; cpu.pf = (f >> 2) & 1; cpu.zf = (f >> 6) & 1;
	cpu.sf = (f >> 7) & 1; cpu.df = (f >> 10) & 1; cpu.of = (f >> 11) & 1;
}

/* ---------------------------------------------------------------- flags */

#define MASK(bits) ((bits) == 32 ? 0xffffffffu : ((1u << (bits)) - 1))
#define SIGN(bits) (1u << ((bits) - 1))

static inline uint32_t parity8(uint32_t v)
{
	v &= 0xff;
	v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
	return (~v) & 1;
}

static inline void szp(uint32_t r, int bits)
{
	cpu.zf = (r & MASK(bits)) == 0;
	cpu.sf = (r & SIGN(bits)) != 0;
	cpu.pf = parity8(r);
}

static inline uint32_t f_add(uint32_t a, uint32_t b, uint32_t c, int bits)
{
	a &= MASK(bits); b &= MASK(bits);
	uint64_t w = (uint64_t)a + b + c;
	uint32_t r = (uint32_t)w & MASK(bits);
	cpu.cf = (uint32_t)(w >> bits) & 1;
	cpu.of = (((a ^ r) & (b ^ r)) & SIGN(bits)) != 0;
	szp(r, bits);
	return r;
}

static inline uint32_t f_sub(uint32_t a, uint32_t b, uint32_t c, int bits)
{
	a &= MASK(bits); b &= MASK(bits);
	uint32_t r = (a - b - c) & MASK(bits);
	cpu.cf = (uint64_t)a < (uint64_t)b + c;
	cpu.of = (((a ^ b) & (a ^ r)) & SIGN(bits)) != 0;
	szp(r, bits);
	return r;
}

static inline uint32_t f_logic(uint32_t r, int bits)
{
	r &= MASK(bits);
	cpu.cf = cpu.of = 0;
	szp(r, bits);
	return r;
}

static inline uint32_t f_inc(uint32_t a, int bits)
{
	uint32_t r = (a + 1) & MASK(bits);
	cpu.of = r == SIGN(bits);
	szp(r, bits);
	return r;
}

static inline uint32_t f_dec(uint32_t a, int bits)
{
	uint32_t r = (a - 1) & MASK(bits);
	cpu.of = (a & MASK(bits)) == SIGN(bits);
	szp(r, bits);
	return r;
}

static inline uint32_t f_neg(uint32_t a, int bits)
{
	a &= MASK(bits);
	uint32_t r = (0u - a) & MASK(bits);
	cpu.cf = a != 0;
	cpu.of = a == SIGN(bits);
	szp(r, bits);
	return r;
}

static inline uint32_t f_shl(uint32_t a, uint32_t n, int bits)
{
	n &= 31;
	a &= MASK(bits);
	if (!n)
		return a;
	uint64_t w = (uint64_t)a << n;
	uint32_t r = (uint32_t)w & MASK(bits);
	cpu.cf = n <= (uint32_t)bits ? (uint32_t)(w >> bits) & 1 : 0;
	cpu.of = ((r & SIGN(bits)) != 0) ^ cpu.cf;
	szp(r, bits);
	return r;
}

static inline uint32_t f_shr(uint32_t a, uint32_t n, int bits)
{
	n &= 31;
	a &= MASK(bits);
	if (!n)
		return a;
	uint32_t r = n >= (uint32_t)bits ? 0 : a >> n;
	cpu.cf = n <= (uint32_t)bits ? (a >> (n - 1)) & 1 : 0;
	cpu.of = (a & SIGN(bits)) != 0;
	szp(r, bits);
	return r;
}

static inline uint32_t f_sar(uint32_t a, uint32_t n, int bits)
{
	n &= 31;
	a &= MASK(bits);
	if (!n)
		return a;
	int32_t s = (int32_t)(a << (32 - bits)) >> (32 - bits);
	uint32_t r;
	if (n >= (uint32_t)bits) {
		r = s < 0 ? MASK(bits) : 0;
		cpu.cf = s < 0;
	} else {
		r = (uint32_t)(s >> n) & MASK(bits);
		cpu.cf = (uint32_t)(s >> (n - 1)) & 1;
	}
	cpu.of = 0;
	szp(r, bits);
	return r;
}

static inline uint32_t f_rol(uint32_t a, uint32_t n, int bits)
{
	a &= MASK(bits);
	if (!(n & 31))
		return a;
	n = (n & 31) % bits;
	uint32_t r = n ? ((a << n) | (a >> (bits - n))) & MASK(bits) : a;
	cpu.cf = r & 1;
	cpu.of = ((r & SIGN(bits)) != 0) ^ cpu.cf;
	return r;
}

static inline uint32_t f_ror(uint32_t a, uint32_t n, int bits)
{
	a &= MASK(bits);
	if (!(n & 31))
		return a;
	n = (n & 31) % bits;
	uint32_t r = n ? ((a >> n) | (a << (bits - n))) & MASK(bits) : a;
	cpu.cf = (r & SIGN(bits)) != 0;
	cpu.of = ((r >> (bits - 1)) ^ (r >> (bits - 2))) & 1;
	return r;
}

static inline uint32_t f_rcl(uint32_t a, uint32_t n, int bits)
{
	n = (n & 31) % (bits + 1);
	a &= MASK(bits);
	for (uint32_t i = 0; i < n; i++) {
		uint32_t c = (a & SIGN(bits)) != 0;
		a = ((a << 1) | cpu.cf) & MASK(bits);
		cpu.cf = c;
	}
	if (n)
		cpu.of = ((a & SIGN(bits)) != 0) ^ cpu.cf;
	return a;
}

static inline uint32_t f_rcr(uint32_t a, uint32_t n, int bits)
{
	n = (n & 31) % (bits + 1);
	a &= MASK(bits);
	if (n)
		cpu.of = ((a & SIGN(bits)) != 0) ^ cpu.cf;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t c = a & 1;
		a = (a >> 1) | (cpu.cf ? SIGN(bits) : 0);
		cpu.cf = c;
	}
	return a;
}

static inline uint32_t f_shld(uint32_t a, uint32_t b, uint32_t n, int bits)
{
	n &= 31;
	if (!n)
		return a & MASK(bits);
	uint64_t w = ((uint64_t)(a & MASK(bits)) << bits) | (b & MASK(bits));
	uint32_t r = (uint32_t)(w >> (bits - n)) & MASK(bits);
	cpu.cf = (uint32_t)(w >> (2 * bits - n)) & 1;
	szp(r, bits);
	return r;
}

static inline uint32_t f_shrd(uint32_t a, uint32_t b, uint32_t n, int bits)
{
	n &= 31;
	if (!n)
		return a & MASK(bits);
	uint64_t w = ((uint64_t)(b & MASK(bits)) << bits) | (a & MASK(bits));
	uint32_t r = (uint32_t)(w >> n) & MASK(bits);
	cpu.cf = (uint32_t)(w >> (n - 1)) & 1;
	szp(r, bits);
	return r;
}

static inline uint32_t bswap32(uint32_t v)
{
	return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24);
}

/* bt [mem], reg: the bit offset is signed and may reach past the dword */
static inline uint32_t bt_mem(uint32_t addr, uint32_t bit)
{
	int32_t b = (int32_t)bit;
	uint32_t a = addr + (uint32_t)((b >> 3) & ~3);
	return (rd32(a) >> (b & 31)) & 1;
}

/* ---------------------------------------------------------------- mul/div */

static inline void mul8(uint32_t v)
{
	uint32_t r = (cpu.r[EAX] & 0xff) * (v & 0xff);
	set16(EAX, r);
	cpu.cf = cpu.of = (r >> 8) != 0;
}

static inline void mul16(uint32_t v)
{
	uint32_t r = (cpu.r[EAX] & 0xffff) * (v & 0xffff);
	set16(EAX, r);
	set16(EDX, r >> 16);
	cpu.cf = cpu.of = (r >> 16) != 0;
}

static inline void mul32(uint32_t v)
{
	uint64_t r = (uint64_t)cpu.r[EAX] * v;
	cpu.r[EAX] = (uint32_t)r;
	cpu.r[EDX] = (uint32_t)(r >> 32);
	cpu.cf = cpu.of = cpu.r[EDX] != 0;
}

static inline void imul8(uint32_t v)
{
	int32_t r = (int8_t)cpu.r[EAX] * (int8_t)v;
	set16(EAX, (uint32_t)r);
	cpu.cf = cpu.of = r != (int8_t)r;
}

static inline void imul16(uint32_t v)
{
	int32_t r = (int16_t)cpu.r[EAX] * (int16_t)v;
	set16(EAX, (uint32_t)r);
	set16(EDX, (uint32_t)r >> 16);
	cpu.cf = cpu.of = r != (int16_t)r;
}

static inline void imul32(uint32_t v)
{
	int64_t r = (int64_t)(int32_t)cpu.r[EAX] * (int32_t)v;
	cpu.r[EAX] = (uint32_t)r;
	cpu.r[EDX] = (uint32_t)((uint64_t)r >> 32);
	cpu.cf = cpu.of = r != (int32_t)r;
}

static inline uint32_t f_imul2(uint32_t a, uint32_t b, int bits)
{
	int64_t r;
	if (bits == 16)
		r = (int64_t)(int16_t)a * (int16_t)b;
	else
		r = (int64_t)(int32_t)a * (int32_t)b;
	uint32_t t = (uint32_t)r & MASK(bits);
	int64_t back = bits == 16 ? (int16_t)t : (int32_t)t;
	cpu.cf = cpu.of = back != r;
	return t;
}

void x86_divide_error(void);

static inline void div8(uint32_t v)
{
	v &= 0xff;
	uint32_t a = cpu.r[EAX] & 0xffff;
	if (!v || a / v > 0xff) { x86_divide_error(); return; }
	set8l(EAX, a / v);
	set8h(EAX, a % v);
}

static inline void div16(uint32_t v)
{
	v &= 0xffff;
	uint32_t a = ((cpu.r[EDX] & 0xffff) << 16) | (cpu.r[EAX] & 0xffff);
	if (!v || a / v > 0xffff) { x86_divide_error(); return; }
	set16(EAX, a / v);
	set16(EDX, a % v);
}

static inline void div32(uint32_t v)
{
	uint64_t a = ((uint64_t)cpu.r[EDX] << 32) | cpu.r[EAX];
	if (!v || a / v > 0xffffffffu) { x86_divide_error(); return; }
	cpu.r[EAX] = (uint32_t)(a / v);
	cpu.r[EDX] = (uint32_t)(a % v);
}

static inline void idiv8(uint32_t v)
{
	int32_t d = (int8_t)v, a = (int16_t)cpu.r[EAX];
	if (!d || a / d > 127 || a / d < -128) { x86_divide_error(); return; }
	set8l(EAX, (uint32_t)(a / d));
	set8h(EAX, (uint32_t)(a % d));
}

static inline void idiv16(uint32_t v)
{
	int32_t d = (int16_t)v;
	int32_t a = (int32_t)(((cpu.r[EDX] & 0xffff) << 16) | (cpu.r[EAX] & 0xffff));
	if (!d || a / d > 32767 || a / d < -32768) { x86_divide_error(); return; }
	set16(EAX, (uint32_t)(a / d));
	set16(EDX, (uint32_t)(a % d));
}

static inline void idiv32(uint32_t v)
{
	int64_t d = (int32_t)v;
	int64_t a = (int64_t)(((uint64_t)cpu.r[EDX] << 32) | cpu.r[EAX]);
	if (!d || a / d > 0x7fffffff || a / d < -0x7fffffffLL - 1) { x86_divide_error(); return; }
	cpu.r[EAX] = (uint32_t)(a / d);
	cpu.r[EDX] = (uint32_t)(a % d);
}

/* ---------------------------------------------------------------- strings */

enum { REP_NONE, REP_PLAIN, REP_E, REP_NE };

static inline uint32_t sz_get(uint32_t a, int sz)
{
	return sz == 1 ? rd8(a) : sz == 2 ? rd16(a) : rd32(a);
}

static inline void sz_put(uint32_t a, uint32_t v, int sz)
{
	if (sz == 1) wr8(a, v); else if (sz == 2) wr16(a, v); else wr32(a, v);
}

static inline uint32_t acc_get(int sz)
{
	return cpu.r[EAX] & MASK(sz * 8);
}

#define STR_STEP(sz) (cpu.df ? -(uint32_t)(sz) : (uint32_t)(sz))

static inline void str_movs(int sz, int rep)
{
	if (rep == REP_NONE) {
		sz_put(cpu.r[EDI], sz_get(cpu.r[ESI], sz), sz);
		cpu.r[ESI] += STR_STEP(sz); cpu.r[EDI] += STR_STEP(sz);
		return;
	}
	if (!cpu.df && cpu.r[ECX]) {
		uint32_t n = cpu.r[ECX] * (uint32_t)sz;
		uint8_t *d = x86_ptr(cpu.r[EDI], n), *s = x86_ptr(cpu.r[ESI], n);
		/* byte-forward copy: overlapping moves replicate as on x86 */
		if (d > s && d < s + n)
			for (uint32_t i = 0; i < n; i++) d[i] = s[i];
		else
			memmove(d, s, n);
		cpu.r[ESI] += n; cpu.r[EDI] += n; cpu.r[ECX] = 0;
		return;
	}
	while (cpu.r[ECX]) {
		sz_put(cpu.r[EDI], sz_get(cpu.r[ESI], sz), sz);
		cpu.r[ESI] += STR_STEP(sz); cpu.r[EDI] += STR_STEP(sz);
		cpu.r[ECX]--;
	}
}

static inline void str_stos(int sz, int rep)
{
	if (rep == REP_NONE) {
		sz_put(cpu.r[EDI], acc_get(sz), sz);
		cpu.r[EDI] += STR_STEP(sz);
		return;
	}
	while (cpu.r[ECX]) {
		sz_put(cpu.r[EDI], acc_get(sz), sz);
		cpu.r[EDI] += STR_STEP(sz);
		cpu.r[ECX]--;
	}
}

static inline void str_lods(int sz, int rep)
{
	do {
		uint32_t v = sz_get(cpu.r[ESI], sz);
		if (sz == 1) set8l(EAX, v); else if (sz == 2) set16(EAX, v); else cpu.r[EAX] = v;
		cpu.r[ESI] += STR_STEP(sz);
		if (rep == REP_NONE)
			return;
	} while (--cpu.r[ECX]);
}

static inline void str_scas(int sz, int rep)
{
	if (rep == REP_NONE) {
		f_sub(acc_get(sz), sz_get(cpu.r[EDI], sz), 0, sz * 8);
		cpu.r[EDI] += STR_STEP(sz);
		return;
	}
	while (cpu.r[ECX]) {
		f_sub(acc_get(sz), sz_get(cpu.r[EDI], sz), 0, sz * 8);
		cpu.r[EDI] += STR_STEP(sz);
		cpu.r[ECX]--;
		if (rep == REP_E ? !cpu.zf : cpu.zf)
			break;
	}
}

static inline void str_cmps(int sz, int rep)
{
	if (rep == REP_NONE) {
		f_sub(sz_get(cpu.r[ESI], sz), sz_get(cpu.r[EDI], sz), 0, sz * 8);
		cpu.r[ESI] += STR_STEP(sz); cpu.r[EDI] += STR_STEP(sz);
		return;
	}
	while (cpu.r[ECX]) {
		f_sub(sz_get(cpu.r[ESI], sz), sz_get(cpu.r[EDI], sz), 0, sz * 8);
		cpu.r[ESI] += STR_STEP(sz); cpu.r[EDI] += STR_STEP(sz);
		cpu.r[ECX]--;
		if (rep == REP_E ? !cpu.zf : cpu.zf)
			break;
	}
}

/* ---------------------------------------------------------------- host */

/* Lifted code: run from pc until it returns to X86_RET_SENTINEL. */
void x86_exec(uint32_t pc);

/* Called by lifted code on backward jumps: runs due timer ticks and
 * presents the screen, as the native timer thread and DirectDraw would. */
extern int x86_poll_countdown;
extern uint32_t x86_poll_pc;
void x86_poll(void);
#define X86_POLL(pc) do { if (--x86_poll_countdown <= 0) { x86_poll_pc = (pc); x86_poll(); } } while (0)

/* Load the exe image into emulated memory. */
int x86_load_image(const char *exe_path);
/* Call a lifted (or HLE) function with stdcall/cdecl args; returns EAX.
 * Saves and restores the caller's registers (for callbacks). */
uint32_t x86_call(uint32_t addr, int nargs, const uint32_t *args);

/* Emulated heap */
uint32_t x86_malloc(uint32_t size);
void x86_free(uint32_t addr);

#endif
