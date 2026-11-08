/*
 * Lockstep check of the two backends (recomp builds, NANPA2_LOCKSTEP=1).
 *
 * The recompiled code runs the game as usual. At the start of each of its
 * basic blocks (X86_BLOCK, emitted by tools/recomp.py with the block's
 * instruction count) the interpreter first runs the same instructions from
 * the same state as a shadow: on a copy of the registers, with its memory
 * writes held in a journal that its reads consult, and without host calls.
 * Then the recompiled block runs, its writes logged (X86_WTRACK). At the
 * next check point the shadow's prediction is compared with what the
 * recompiled code did:
 *   next block start  pc, registers, flags, instruction count, poll
 *                     countdown, every byte either side wrote
 *   hle_call entry    the same, pc = the HLE target (before the HLE runs)
 *   x86_poll entry    the same, poll at the same branch (before the timer)
 *   return to host    the same, pc = X86_RET_SENTINEL
 * A host call in the middle of a block (a probe calling game code through
 * x86_call) drops that block's check. The first divergence stops the run
 * with both states; NANPA2_INTERP=1 instead runs the interpreter alone.
 */
#include "interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int x86_lockstep_on;
int x86_wtrack_on;

#define MEM_SIZE (MEM_END - MEM_BASE)

/* shadow journal: one bit per byte of emulated memory, the values, and
 * the offsets written */
static uint8_t *jbits, *jval;
static uint32_t *jlog;
static size_t jlog_n, jlog_cap;

/* writes by the recompiled block */
static struct wr { uint32_t a, n; } *mlog;
static size_t mlog_n, mlog_cap;

void x86_wtrack(uint32_t a, uint32_t n)
{
	if (mlog_n == mlog_cap) {
		mlog_cap = mlog_cap ? mlog_cap * 2 : 4096;
		mlog = realloc(mlog, mlog_cap * sizeof *mlog);
		if (!mlog)
			abort();
	}
	mlog[mlog_n].a = a;
	mlog[mlog_n].n = n;
	mlog_n++;
}

static inline int jhas(uint32_t o)
{
	return (jbits[o >> 3] >> (o & 7)) & 1;
}

static inline uint32_t j_rd8(uint32_t a)
{
	uint8_t *p = x86_ptr(a, 1);
	uint32_t o = a - MEM_BASE;
	return jhas(o) ? jval[o] : *p;
}

static inline void j_wr8(uint32_t a, uint32_t v)
{
	x86_ptr(a, 1);
	uint32_t o = a - MEM_BASE;
	if (!jhas(o)) {
		jbits[o >> 3] |= (uint8_t)(1u << (o & 7));
		if (jlog_n == jlog_cap) {
			jlog_cap = jlog_cap ? jlog_cap * 2 : 4096;
			jlog = realloc(jlog, jlog_cap * sizeof *jlog);
			if (!jlog)
				abort();
		}
		jlog[jlog_n++] = o;
	}
	jval[o] = (uint8_t)v;
}

static inline uint32_t j_rd16(uint32_t a) { return j_rd8(a) | j_rd8(a + 1) << 8; }
static inline uint32_t j_rd32(uint32_t a) { return j_rd16(a) | j_rd16(a + 2) << 16; }
static inline void j_wr16(uint32_t a, uint32_t v) { j_wr8(a, v); j_wr8(a + 1, v >> 8); }
static inline void j_wr32(uint32_t a, uint32_t v) { j_wr16(a, v); j_wr16(a + 2, v >> 16); }

static inline uint32_t j_get(uint32_t a, int sz) { return sz == 1 ? j_rd8(a) : sz == 2 ? j_rd16(a) : j_rd32(a); }
static inline void j_put(uint32_t a, uint32_t v, int sz)
{
	if (sz == 1) j_wr8(a, v); else if (sz == 2) j_wr16(a, v); else j_wr32(a, v);
}

/* the string instructions of x86.h, through the journal */
static void j_movs(int sz, int rep)
{
	if (rep == REP_NONE) {
		j_put(cpu.r[EDI], j_get(cpu.r[ESI], sz), sz);
		cpu.r[ESI] += STR_STEP(sz); cpu.r[EDI] += STR_STEP(sz);
		return;
	}
	if (!cpu.df && cpu.r[ECX]) {
		/* x86.h: memmove, or byte-forward when the destination overlaps
		 * the source from above; both equal a byte-forward copy */
		uint32_t n = cpu.r[ECX] * (uint32_t)sz;
		x86_ptr(cpu.r[EDI], n);
		x86_ptr(cpu.r[ESI], n);
		for (uint32_t i = 0; i < n; i++)
			j_wr8(cpu.r[EDI] + i, j_rd8(cpu.r[ESI] + i));
		cpu.r[ESI] += n; cpu.r[EDI] += n; cpu.r[ECX] = 0;
		return;
	}
	while (cpu.r[ECX]) {
		j_put(cpu.r[EDI], j_get(cpu.r[ESI], sz), sz);
		cpu.r[ESI] += STR_STEP(sz); cpu.r[EDI] += STR_STEP(sz);
		cpu.r[ECX]--;
	}
}

static void j_stos(int sz, int rep)
{
	if (rep == REP_NONE) {
		j_put(cpu.r[EDI], acc_get(sz), sz);
		cpu.r[EDI] += STR_STEP(sz);
		return;
	}
	while (cpu.r[ECX]) {
		j_put(cpu.r[EDI], acc_get(sz), sz);
		cpu.r[EDI] += STR_STEP(sz);
		cpu.r[ECX]--;
	}
}

static void j_lods(int sz, int rep)
{
	do {
		uint32_t v = j_get(cpu.r[ESI], sz);
		if (sz == 1) set8l(EAX, v); else if (sz == 2) set16(EAX, v); else cpu.r[EAX] = v;
		cpu.r[ESI] += STR_STEP(sz);
		if (rep == REP_NONE)
			return;
	} while (--cpu.r[ECX]);
}

static void j_scas(int sz, int rep)
{
	if (rep == REP_NONE) {
		f_sub(acc_get(sz), j_get(cpu.r[EDI], sz), 0, sz * 8);
		cpu.r[EDI] += STR_STEP(sz);
		return;
	}
	while (cpu.r[ECX]) {
		f_sub(acc_get(sz), j_get(cpu.r[EDI], sz), 0, sz * 8);
		cpu.r[EDI] += STR_STEP(sz);
		cpu.r[ECX]--;
		if (rep == REP_E ? !cpu.zf : cpu.zf)
			break;
	}
}

static void j_cmps(int sz, int rep)
{
	if (rep == REP_NONE) {
		f_sub(j_get(cpu.r[ESI], sz), j_get(cpu.r[EDI], sz), 0, sz * 8);
		cpu.r[ESI] += STR_STEP(sz); cpu.r[EDI] += STR_STEP(sz);
		return;
	}
	while (cpu.r[ECX]) {
		f_sub(j_get(cpu.r[ESI], sz), j_get(cpu.r[EDI], sz), 0, sz * 8);
		cpu.r[ESI] += STR_STEP(sz); cpu.r[EDI] += STR_STEP(sz);
		cpu.r[ECX]--;
		if (rep == REP_E ? !cpu.zf : cpu.zf)
			break;
	}
}

/* shadow counters */
static uint64_t sh_icount;
static int sh_countdown;
static uint32_t sh_poll_at;

#define IFN(name) s_##name
#define SHADOW 1
#define MRD8(a) j_rd8(a)
#define MRD16(a) j_rd16(a)
#define MRD32(a) j_rd32(a)
#define MWR8(a, v) j_wr8((a), (v))
#define MWR16(a, v) j_wr16((a), (v))
#define MWR32(a, v) j_wr32((a), (v))
#define COUNT(n) (sh_icount += (n))
#define POLL(at) do { if (--sh_countdown <= 0) sh_poll_at = (at); } while (0)
#define STR_MOVS(sz, rep) j_movs((sz), (rep))
#define STR_STOS(sz, rep) j_stos((sz), (rep))
#define STR_LODS(sz, rep) j_lods((sz), (rep))
#define STR_SCAS(sz, rep) j_scas((sz), (rep))
#define STR_CMPS(sz, rep) j_cmps((sz), (rep))
#include "interp_core.h"

/* ---------------------------------------------------------------- check */

static int pending;
static struct cpu exp_cpu;
static uint32_t exp_pc, exp_poll_at, blk_pc, blk_n;
static uint64_t exp_icount;
static int exp_countdown;

static uint64_t n_blocks, n_insns, n_hle, n_poll, n_ret, n_dropped, n_bytes;
static uint64_t next_report = 100000000;
static int diverged;

static void journal_clear(void)
{
	for (size_t i = 0; i < jlog_n; i++)
		jbits[jlog[i] >> 3] = 0;
	jlog_n = 0;
}

static void dump(const char *name, const struct cpu *c)
{
	fprintf(stderr, "  %s: eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x"
		" cf=%u zf=%u sf=%u of=%u pf=%u df=%u\n", name,
		c->r[EAX], c->r[ECX], c->r[EDX], c->r[EBX], c->r[ESP], c->r[EBP], c->r[ESI], c->r[EDI],
		c->cf, c->zf, c->sf, c->of, c->pf, c->df);
}

static void report(void)
{
	fprintf(stderr, "lockstep: %llu blocks, %llu instructions compared (%llu at HLE calls, %llu at polls,"
		" %llu at returns, %llu bytes written), %llu dropped (host call mid-block)%s\n",
		(unsigned long long)n_blocks, (unsigned long long)n_insns, (unsigned long long)n_hle,
		(unsigned long long)n_poll, (unsigned long long)n_ret, (unsigned long long)n_bytes,
		(unsigned long long)n_dropped, diverged ? "" : ", no divergence");
}

static const char *const kind_name[] = { "block start", "HLE call", "poll", "return", "nest" };

void x86_lockstep_point(int kind, uint32_t pc)
{
	if (!pending) {
		mlog_n = 0;
		return;
	}
	pending = 0;
	if (kind == LS_NEST) {
		n_dropped++;
		journal_clear();
		mlog_n = 0;
		return;
	}
	const char *why = NULL;
	uint32_t at = 0;
	switch (kind) {
	case LS_BLOCK: case LS_HLE: case LS_RET:
		if (exp_pc != pc)
			why = "next pc";
		else if (exp_poll_at)
			why = "interpreter polled, recompiled code did not";
		break;
	case LS_POLL:
		if (exp_poll_at != pc)
			why = "poll at a different branch";
		break;
	}
	if (!why) {
		for (int i = 0; i < 8; i++)
			if (cpu.r[i] != exp_cpu.r[i])
				why = "registers";
		if (cpu.cf != exp_cpu.cf || cpu.zf != exp_cpu.zf || cpu.sf != exp_cpu.sf ||
		    cpu.of != exp_cpu.of || cpu.pf != exp_cpu.pf || cpu.df != exp_cpu.df)
			why = "flags";
		if (x86_icount != exp_icount)
			why = "instruction count";
		if (x86_poll_countdown != exp_countdown)
			why = "poll countdown";
	}
	for (size_t i = 0; !why && i < jlog_n; i++)
		if (jval[jlog[i]] != x86_mem[jlog[i]]) {
			why = "memory differs";
			at = MEM_BASE + jlog[i];
		}
	for (size_t i = 0; !why && i < mlog_n; i++)
		for (uint32_t k = 0; k < mlog[i].n; k++) {
			uint32_t o = mlog[i].a + k - MEM_BASE;
			if (!jhas(o)) {
				why = "written by the recompiled code only";
				at = mlog[i].a + k;
				break;
			}
		}
	if (why) {
		diverged = 1;
		fprintf(stderr, "lockstep: DIVERGENCE at %s %08x: %s\n", kind_name[kind], pc, why);
		fprintf(stderr, "  block %08x, %u instructions; interpreter next pc %08x, poll at %08x\n",
			blk_pc, blk_n, exp_pc, exp_poll_at);
		dump("recomp", &cpu);
		dump("interp", &exp_cpu);
		fprintf(stderr, "  icount recomp %llu interp %llu; poll countdown recomp %d interp %d\n",
			(unsigned long long)x86_icount, (unsigned long long)exp_icount, x86_poll_countdown,
			exp_countdown);
		if (at)
			fprintf(stderr, "  byte %08x: recomp %02x interp %s%02x\n", at, x86_mem[at - MEM_BASE],
				jhas(at - MEM_BASE) ? "" : "(unwritten) ", jhas(at - MEM_BASE) ? jval[at - MEM_BASE] : x86_mem[at - MEM_BASE]);
		report();
		x86_trap(blk_pc, "lockstep divergence");
	}
	n_blocks++;
	n_insns += blk_n;
	n_bytes += jlog_n;
	n_hle += kind == LS_HLE;
	n_poll += kind == LS_POLL;
	n_ret += kind == LS_RET;
	journal_clear();
	mlog_n = 0;
	if (n_insns >= next_report) {
		next_report += 100000000;
		report();
	}
}

void x86_lockstep_block(uint32_t pc, uint32_t n)
{
	x86_lockstep_point(LS_BLOCK, pc);
	struct cpu saved = cpu;
	sh_icount = x86_icount;
	sh_countdown = x86_poll_countdown;
	sh_poll_at = 0;
	exp_pc = s_run(pc, n);
	exp_cpu = cpu;
	cpu = saved;
	exp_icount = sh_icount;
	exp_countdown = sh_countdown;
	exp_poll_at = sh_poll_at;
	blk_pc = pc;
	blk_n = n;
	pending = 1;
	mlog_n = 0;
}

void x86_lockstep_init(void)
{
	const char *e = getenv("NANPA2_INTERP");
	if (e && *e && strcmp(e, "0"))
		x86_use_interp = 1;
	e = getenv("NANPA2_LOCKSTEP");
	if (!e || !*e || !strcmp(e, "0"))
		return;
	x86_interp_init();
	jbits = calloc(1, MEM_SIZE / 8);
	jval = malloc(MEM_SIZE);
	if (!jbits || !jval) {
		fprintf(stderr, "lockstep: out of memory\n");
		exit(1);
	}
	x86_use_interp = 0;
	x86_lockstep_on = 1;
	x86_wtrack_on = 1;
	atexit(report);
	fprintf(stderr, "lockstep: interpreter shadows every recompiled block\n");
}
