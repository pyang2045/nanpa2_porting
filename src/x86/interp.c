/*
 * x86 interpreter backend: runs the game code of the player's nanpa2.exe
 * (0x401000-0x407800, loaded by x86_load_image) by decoding it at run time.
 * Hand-written; it holds no bytes of the exe. The instruction set is the
 * subset tools/recomp.py's lifter handles, which covers the game code;
 * anything else stops with the address and bytes (x86_unimplemented).
 *
 * Everything else the code reaches (imports, COM methods, the C runtime and
 * the overrides of src/hle/overrides.h) goes to hle_call, as in the
 * recompiled code; polls (X86_POLL) and instruction counts (X86_COUNT) are
 * the recompile's, so pacing and the message pump behave the same.
 */
#include "interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

uint8_t x86_interp_override[CODE_LEN];
struct dec dec_cache[CODE_LEN];
uint64_t x86_interp_steps;
uint64_t x86_native_total;
int x86_native_hold;
#define NATIVE_END 0x004077adu
static uint32_t *native_hist; /* per code byte; NULL = not counting */
static uint8_t *native_entry; /* call targets seen in native game code */

static const uint32_t overrides[] = {
#define HLE_OVERRIDE(addr, what) addr,
#include "hle/overrides.h"
#undef HLE_OVERRIDE
};

static const uint32_t game_overrides[] = {
#define HLE_GAME_OVERRIDE(addr, what) addr,
#include "hle/overrides.h"
#undef HLE_GAME_OVERRIDE
	0
};

static const uint32_t jump_overrides[] = {
#define HLE_GAME_JUMP(addr, what) addr,
#include "hle/overrides.h"
#undef HLE_GAME_JUMP
	0
};

static const uint32_t probes[] = {
#define HLE_PROBE(addr, what) addr,
#include "hle/overrides.h"
#undef HLE_PROBE
};

void x86_interp_init(void)
{
	static int done;
	if (done)
		return;
	done = 1;
	for (size_t i = 0; i < sizeof overrides / sizeof overrides[0]; i++)
		if (overrides[i] - CODE_START < CODE_LEN)
			x86_interp_override[overrides[i] - CODE_START] = OVR_HLE;
	for (size_t i = 0; game_overrides[i]; i++)
		if (game_overrides[i] - CODE_START < CODE_LEN)
			x86_interp_override[game_overrides[i] - CODE_START] = OVR_GAME;
	for (size_t i = 0; jump_overrides[i]; i++)
		if (jump_overrides[i] - CODE_START < CODE_LEN)
			x86_interp_override[jump_overrides[i] - CODE_START] = OVR_JUMP;
}

_Noreturn void x86_unimplemented(uint32_t pc)
{
	char msg[96];
	int n = snprintf(msg, sizeof msg, "instruction not implemented by the interpreter:");
	for (uint32_t i = 0; i < 8 && n < (int)sizeof msg - 4; i++)
		n += snprintf(msg + n, sizeof msg - (size_t)n, " %02x", rd8(pc + i));
	x86_trap(pc, msg);
}

/* ---------------------------------------------------------------- decoder */

static int has_modrm(uint32_t op)
{
	if (op < 0x40)
		return (op & 7) < 4;
	if (op >= 0x100) {
		uint32_t o = op & 0xff;
		return (o >= 0x90 && o <= 0x9f) || o == 0xa3 || o == 0xa4 || o == 0xa5 || o == 0xab ||
		       o == 0xac || o == 0xad || o == 0xaf || o == 0xb3 || o == 0xb6 || o == 0xb7 ||
		       o == 0xba || o == 0xbb || o == 0xbe || o == 0xbf;
	}
	switch (op) {
	case 0x69: case 0x6b:
	case 0x80: case 0x81: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
	case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8d: case 0x8f:
	case 0xc0: case 0xc1: case 0xc6: case 0xc7:
	case 0xd0: case 0xd1: case 0xd2: case 0xd3:
	case 0xf6: case 0xf7: case 0xfe: case 0xff:
		return 1;
	}
	return 0;
}

/* immediate size in bytes; negative = sign-extended imm8; 0x40 = rel8,
 * 0x41 = rel32 */
enum { REL8 = 0x40, REL32 = 0x41 };

static int imm_kind(uint32_t op, int osz, int reg)
{
	if (op < 0x40)
		return (op & 7) == 4 ? 1 : (op & 7) == 5 ? osz : 0;
	if (op >= 0x100) {
		uint32_t o = op & 0xff;
		if (o >= 0x80 && o <= 0x8f)
			return REL32;
		return o == 0xa4 || o == 0xac || o == 0xba ? 1 : 0;
	}
	if (op >= 0x70 && op <= 0x7f)
		return REL8;
	if (op >= 0xb0 && op <= 0xb7)
		return 1;
	if (op >= 0xb8 && op <= 0xbf)
		return osz;
	switch (op) {
	case 0x68: case 0x69: case 0x81: case 0xa9: case 0xc7: return osz;
	case 0x6a: case 0x6b: case 0x83: return -1;
	case 0x80: case 0xa8: case 0xc0: case 0xc1: case 0xc6: return 1;
	case 0xa0: case 0xa1: case 0xa2: case 0xa3: return 4; /* moffs32 */
	case 0xc2: return 2;
	case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xeb: return REL8;
	case 0xe8: case 0xe9: return REL32;
	case 0xf6: return reg < 2 ? 1 : 0;
	case 0xf7: return reg < 2 ? osz : 0;
	}
	return 0;
}

static int is_probe(uint32_t pc)
{
	for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++)
		if (probes[i] == pc)
			return 1;
	return 0;
}

const struct dec *x86_decode(uint32_t pc)
{
	struct dec d = { 0 };
	uint32_t p = pc;
	if (x86_code_absent)
		x86_trap(pc, "native game code is not in memory (running from nanpa2.dat); "
			     "this mode needs nanpa2.exe");
	x86_img_ctx = 2;
	d.osz = 4;
	d.base = d.index = NOREG;
	for (;; p++) {
		uint32_t b = rd8(p);
		if (b == 0x66)
			d.osz = 2;
		else if (b == 0xf2 || b == 0xf3)
			d.rep = (uint8_t)b;
		else if (b == 0xf0 || b == 0x26 || b == 0x2e || b == 0x36 || b == 0x3e)
			; /* lock; es/cs/ss/ds: flat memory */
		else if (b == 0x64 || b == 0x65 || b == 0x67)
			x86_unimplemented(pc); /* fs/gs or 16-bit addressing */
		else
			break;
		if (p - pc > 8)
			x86_unimplemented(pc);
	}
	uint32_t op = rd8(p++);
	if (op == 0x0f)
		op = 0x100 | rd8(p++);
	d.op = (uint16_t)op;
	if ((op >= 0x40 && op <= 0x5f) || (op >= 0x90 && op <= 0xbf) || (op >= 0x1c8 && op <= 0x1cf))
		d.reg = (uint8_t)(op & 7);
	if (has_modrm(op)) {
		uint32_t m = rd8(p++);
		uint32_t mod = m >> 6, rm = m & 7;
		d.reg = (uint8_t)((m >> 3) & 7);
		if (mod == 3) {
			d.modreg = 1;
			d.rm = (uint8_t)rm;
		} else {
			if (rm == 4) {
				uint32_t sib = rd8(p++);
				d.scale = (uint8_t)(sib >> 6);
				d.index = ((sib >> 3) & 7) == 4 ? NOREG : (uint8_t)((sib >> 3) & 7);
				d.base = (uint8_t)(sib & 7);
				if (d.base == 5 && mod == 0) {
					d.base = NOREG;
					d.disp = (int32_t)rd32(p);
					p += 4;
				}
			} else if (rm == 5 && mod == 0) {
				d.disp = (int32_t)rd32(p);
				p += 4;
			} else {
				d.base = (uint8_t)rm;
			}
			if (mod == 1) {
				d.disp = (int8_t)rd8(p);
				p += 1;
			} else if (mod == 2) {
				d.disp = (int32_t)rd32(p);
				p += 4;
			}
		}
	}
	int k = imm_kind(op, d.osz, d.reg);
	if ((k == REL32 || op == 0xe8 || op == 0xe9) && d.osz == 2)
		x86_unimplemented(pc); /* 16-bit branch targets */
	switch (k) {
	case 0: break;
	case 1: d.imm = rd8(p); p += 1; break;
	case -1: d.imm = (uint32_t)(int32_t)(int8_t)rd8(p); p += 1; break;
	case 2: d.imm = rd16(p); p += 2; break;
	case 4: d.imm = rd32(p); p += 4; break;
	case REL8: d.imm = (uint32_t)(int32_t)(int8_t)rd8(p); p += 1; d.imm += p; break;
	case REL32: d.imm = rd32(p); p += 4; d.imm += p; break;
	}
	x86_img_ctx = 1;
	d.len = (uint8_t)(p - pc);
	d.probe = (uint8_t)is_probe(pc);
	dec_cache[pc - CODE_START] = d;
	return &dec_cache[pc - CODE_START];
}

/* ---------------------------------------------------------------- run */

/* wall time in HLE calls from interpreted code (outermost only: an HLE call
 * can run game code, which calls HLE again) */
double x86_interp_hle_secs;
static int hle_depth;

static double now_secs(void)
{
	struct timespec ts;
	timespec_get(&ts, TIME_UTC);
	return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void timed_hle_call(uint32_t pc)
{
	if (hle_depth++) {
		hle_call(pc);
	} else {
		double t0 = now_secs();
		hle_call(pc);
		x86_interp_hle_secs += now_secs() - t0;
	}
	hle_depth--;
}

static uint32_t timed_hle_jump(uint32_t pc)
{
	uint32_t next;
	if (hle_depth++) {
		next = hle_jump(pc);
	} else {
		double t0 = now_secs();
		next = hle_jump(pc);
		x86_interp_hle_secs += now_secs() - t0;
	}
	hle_depth--;
	return next;
}

static inline void native_step(uint32_t pc, const struct dec *d)
{
	if (native_hist && pc - CODE_START < NATIVE_END - CODE_START && !x86_native_hold) {
		native_hist[pc - CODE_START]++;
		x86_native_total++;
		if (d->op == 0xe8 && d->imm - CODE_START < NATIVE_END - CODE_START)
			native_entry[d->imm - CODE_START] = 1;
	}
}

/* functions are named by the nearest entry below: a call target seen, or
 * a unit (reimplemented or not) of src/game/units.c */
struct nfn { uint32_t addr; uint64_t n; };

static int nfn_cmp(const void *a, const void *b)
{
	const struct nfn *x = a, *y = b;
	return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}

static int native_group(struct nfn *f, int cap)
{
	int n = 0;
	uint32_t cur = 0;
	for (uint32_t o = 0; o < NATIVE_END - CODE_START; o++) {
		if (native_entry[o])
			cur = CODE_START + o;
		if (!native_hist[o])
			continue;
		if (!n || f[n - 1].addr != cur) {
			if (n == cap)
				break;
			f[n].addr = cur;
			f[n].n = 0;
			n++;
		}
		f[n - 1].n += native_hist[o];
	}
	return n;
}

void x86_native_report(void)
{
	if (!native_hist)
		return;
	struct nfn f[256];
	int n = native_group(f, 256);
	qsort(f, (size_t)n, sizeof f[0], nfn_cmp);
	fprintf(stderr, "native: %llu game instructions run natively (0x401000-0x4077ac), %d functions\n",
	        (unsigned long long)x86_native_total, n);
	for (int i = 0; i < n; i++)
		fprintf(stderr, "native: %08x %llu\n", f[i].addr, (unsigned long long)f[i].n);
}

void x86_native_summary(char *out, size_t cap)
{
	out[0] = 0;
	if (!native_hist)
		return;
	struct nfn f[256];
	int n = native_group(f, 256);
	qsort(f, (size_t)n, sizeof f[0], nfn_cmp);
	size_t o = (size_t)snprintf(out, cap, "{\"total\": %llu, \"top\": {", (unsigned long long)x86_native_total);
	for (int i = 0; i < n && i < 8 && o < cap; i++)
		o += (size_t)snprintf(out + o, cap - o, "%s\"%08x\": %llu", i ? ", " : "", f[i].addr,
		                      (unsigned long long)f[i].n);
	if (o < cap)
		snprintf(out + o, cap - o, "}}");
}

#define IFN(name) i_##name
#define SHADOW 0
#define MRD8(a) rd8(a)
#define MRD16(a) rd16(a)
#define MRD32(a) rd32(a)
#define MWR8(a, v) wr8((a), (v))
#define MWR16(a, v) wr16((a), (v))
#define MWR32(a, v) wr32((a), (v))
#define COUNT(n) X86_COUNT(n)
#define STEP() (x86_interp_steps++, native_step(pc, d))
#define NATIVE_ENTRY(p) do { if (native_entry && (p) - CODE_START < NATIVE_END - CODE_START) \
	native_entry[(p) - CODE_START] = 1; } while (0)
#define HLE_CALL(pc) timed_hle_call(pc)
#define HLE_JUMP(pc) timed_hle_jump(pc)
#define POLL(at) X86_POLL(at)
#define STR_MOVS(sz, rep) str_movs((sz), (rep))
#define STR_STOS(sz, rep) str_stos((sz), (rep))
#define STR_LODS(sz, rep) str_lods((sz), (rep))
#define STR_SCAS(sz, rep) str_scas((sz), (rep))
#define STR_CMPS(sz, rep) str_cmps((sz), (rep))
#include "interp_core.h"

void x86_interp(uint32_t pc)
{
	x86_interp_init();
	if (!native_hist) {
		static int checked;
		const char *e = getenv("NANPA2_NATIVE_REPORT");
		if (!checked++ && e && *e && *e != '0') {
			native_hist = calloc(NATIVE_END - CODE_START, sizeof *native_hist);
			native_entry = calloc(NATIVE_END - CODE_START, 1);
			for (size_t i = 0; i < sizeof overrides / sizeof overrides[0]; i++)
				if (overrides[i] - CODE_START < NATIVE_END - CODE_START)
					native_entry[overrides[i] - CODE_START] = 1;
			for (size_t i = 0; i < sizeof game_overrides / sizeof game_overrides[0]; i++)
					if (game_overrides[i] - CODE_START < NATIVE_END - CODE_START)
					native_entry[game_overrides[i] - CODE_START] = 1;
			for (size_t i = 0; i < sizeof jump_overrides / sizeof jump_overrides[0]; i++)
					if (jump_overrides[i] - CODE_START < NATIVE_END - CODE_START)
					native_entry[jump_overrides[i] - CODE_START] = 1;
			native_entry[0x40638du - CODE_START] = 1; /* the VM end, reached by jump */
			atexit(x86_native_report);
		}
	}
	if (native_entry && (pc & ~X86_ORACLE_TAG) - CODE_START < NATIVE_END - CODE_START)
		native_entry[(pc & ~X86_ORACLE_TAG) - CODE_START] = 1; /* entered by x86_call/ncall */
	if (x86_is_oracle_pc(pc)) {
		/* the original of a reimplemented function (NANPA2_DIFF) */
		pc &= ~X86_ORACLE_TAG;
		if (pc - CODE_START >= CODE_LEN || x86_interp_override[pc - CODE_START] < OVR_GAME)
			x86_trap(pc, "oracle entry is not an HLE_GAME_OVERRIDE or HLE_GAME_JUMP");
		i_run(pc, 0, pc);
		return;
	}
	i_run(pc, 0, 0);
}

/* The backend switch; here rather than in x86.c, which diff_replay links
 * with its own x86_exec. */
#ifdef NANPA2_HAVE_RECOMP
int x86_use_interp;
#else
int x86_use_interp = 1;
#endif

const char *x86_backend_name(void)
{
#ifdef NANPA2_HAVE_RECOMP
	if (x86_lockstep_on)
		return "lockstep";
#endif
	return x86_use_interp ? "interp" : "recomp";
}

void x86_exec(uint32_t pc)
{
#ifdef NANPA2_HAVE_RECOMP
	if (!x86_use_interp) {
		x86_exec_recomp(pc);
		X86_LOCKSTEP_POINT(LS_RET, X86_RET_SENTINEL);
		return;
	}
#endif
	x86_interp(pc);
}
