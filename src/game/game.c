/*
 * Dispatch of the reimplemented units and the online differential test.
 *
 * NANPA2_DIFF=1: every call of a unit first runs the lifted original
 * (x86_exec(X86_ORACLE_TAG | addr)) with memory tracing, undoes its writes,
 * runs the reimplementation from the same state and compares registers
 * (minus the unit's scratch registers), flags if a caller uses them, and
 * every byte either wrote at or above the entry ESP. Differences are logged
 * as "diff MISMATCH" and the original's result is kept, so the game goes on
 * as the unmodified one would. Nested units run as originals inside an
 * original, as reimplementations inside a reimplementation.
 * NANPA2_DIFF_DIR=DIR also appends each call, deduplicated by its inputs,
 * to DIR/<addr>-<pid>.n2diff for the offline replay (tests/diff_replay.c);
 * NANPA2_DIFF_MAX caps the records per unit (default 40000).
 */
#include "game/game.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "game/diff.h"
#include "game/expr.h"
#include "game/trace.h"
#include "x86/x86.h"

enum { MODE_OFF, MODE_ORACLE, MODE_REIMPL };

struct unit_stats {
	uint64_t calls, mismatches, recorded, oracle_insns, reimpl_insns, native_runs;
	FILE *file;
	uint64_t *seen; /* open-addressing set of input hashes */
	size_t seen_cap, seen_n;
};

static int diff_on = -1;
static const char *diff_dir;
static uint64_t diff_max = 40000;
static int mode = MODE_OFF;
static struct unit_stats *stats;

static void report(void)
{
	for (int i = 0; i < game_unit_count; i++) {
		struct unit_stats *s = &stats[i];
		if (s->native_runs)
			fprintf(stderr, "diff: %08x %s left to the native handler %llu times\n",
			        game_units[i].addr, game_units[i].name, (unsigned long long)s->native_runs);
		if (!s->calls)
			continue;
		fprintf(stderr,
		        "diff: %08x %s calls %llu mismatches %llu recorded %llu insns/call native %.1f reimpl %.1f\n",
		        game_units[i].addr, game_units[i].name, (unsigned long long)s->calls,
		        (unsigned long long)s->mismatches, (unsigned long long)s->recorded,
		        (double)s->oracle_insns / (double)s->calls, (double)s->reimpl_insns / (double)s->calls);
		if (s->file)
			fclose(s->file);
		s->file = NULL;
	}
	fprintf(stderr, "diff: script_eval tokens:");
	for (int t = 0; t < 256; t++)
		if (script_eval_coverage[t])
			fprintf(stderr, " %02x:%llu", t, (unsigned long long)script_eval_coverage[t]);
	fprintf(stderr, "\n");
}

static void diff_init(void)
{
	const char *e = getenv("NANPA2_DIFF");
	diff_on = e && *e && strcmp(e, "0");
	diff_dir = getenv("NANPA2_DIFF_DIR");
	if (diff_dir && !*diff_dir)
		diff_dir = NULL;
	if (diff_dir)
		diff_on = 1;
	if ((e = getenv("NANPA2_DIFF_MAX")))
		diff_max = strtoull(e, NULL, 10);
	if (diff_on) {
		stats = calloc((size_t)game_unit_count, sizeof *stats);
		if (!stats)
			abort();
		atexit(report);
		fprintf(stderr, "diff: comparing %d units against the lifted originals%s%s\n",
		        game_unit_count, diff_dir ? ", records in " : "", diff_dir ? diff_dir : "");
	}
}

/* 1 if the hash is new (and remembers it) */
static int first_seen(struct unit_stats *s, uint64_t h)
{
	if (!h)
		h = 1;
	if (2 * (s->seen_n + 1) > s->seen_cap) {
		size_t cap = s->seen_cap ? 2 * s->seen_cap : 1024;
		uint64_t *t = calloc(cap, sizeof *t);
		if (!t)
			abort();
		for (size_t i = 0; i < s->seen_cap; i++)
			if (s->seen[i])
				for (size_t j = s->seen[i] & (cap - 1);; j = (j + 1) & (cap - 1))
					if (!t[j]) {
						t[j] = s->seen[i];
						break;
					}
		free(s->seen);
		s->seen = t;
		s->seen_cap = cap;
	}
	for (size_t j = h & (s->seen_cap - 1);; j = (j + 1) & (s->seen_cap - 1)) {
		if (s->seen[j] == h)
			return 0;
		if (!s->seen[j]) {
			s->seen[j] = h;
			s->seen_n++;
			return 1;
		}
	}
}

static void record(const struct game_unit *u, struct unit_stats *s, const struct diff_record *rec)
{
	if (!diff_dir || s->recorded >= diff_max || u->calls_native)
		return;
	uint64_t h = diff_input_hash(rec);
	/* the first half of the budget takes every new input, the rest one in
	 * 16, so a long run is sampled to its end */
	if ((s->recorded >= diff_max / 2 && (h >> 40) % 16) || !first_seen(s, h))
		return;
	if (!s->file) {
		char path[1024];
		snprintf(path, sizeof path, "%s/%08x-%ld.n2diff", diff_dir, u->addr, (long)getpid());
		s->file = fopen(path, "wb");
		if (!s->file) {
			fprintf(stderr, "diff: cannot write %s\n", path);
			diff_dir = NULL;
			return;
		}
		fputs(DIFF_MAGIC, s->file);
	}
	if (diff_write(s->file, rec))
		s->recorded++;
}

const uint32_t game_vm_exits[] = { 0x4060f9u, 0x40638du, 0 };

/* Where else a jump unit's oracle ends (x86_stop_at): at any jump unit
 * (the pieces of a handler split at its calls go on at one another), and
 * at the call exits of src/hle/overrides.h, as {callee, return address}
 * with the callee read from the `e8 rel32` before the return address. */
static uint32_t stop_units[256], stop_calls[2 * 64 + 1];

static const uint32_t call_exit_ra[] = {
#define HLE_GAME_CALL_EXIT(ra, what) ra,
#include "hle/overrides.h"
#undef HLE_GAME_CALL_EXIT
	0
};

static void stop_tables(void)
{
	static int done;
	if (done)
		return;
	done = 1;
	int n = 0;
	for (int i = 0; i < game_unit_count; i++)
		if (game_units[i].jump) {
			if (n + 1 >= (int)(sizeof stop_units / sizeof stop_units[0]))
				x86_trap(game_units[i].addr, "game.c: stop_units too small");
			stop_units[n++] = game_units[i].addr;
		}
	stop_units[n] = 0;
	n = 0;
	for (const uint32_t *ra = call_exit_ra; *ra; ra++) {
		const uint8_t *p = x86_ptr(*ra - 5, 5);
		if (p[0] != 0xe8 || 2 * n + 2 >= (int)(sizeof stop_calls / sizeof stop_calls[0]))
			x86_trap(*ra, "game.c: call exit without a call before it");
		uint32_t rel = p[1] | p[2] << 8 | p[3] << 16 | (uint32_t)p[4] << 24;
		stop_calls[2 * n] = *ra + rel;
		stop_calls[2 * n + 1] = *ra;
		n++;
	}
	stop_calls[2 * n] = 0;
}

/* NANPA2_DIFF_POISON=1: after every unit (compared or not) its scratch
 * registers, and the flags if no caller reads them, get garbage, so a
 * register wrongly taken as dead shows up as a divergence or a trap. */
static int poison_on = -1;

static void poison(const struct game_unit *u)
{
	static uint32_t n;
	if (poison_on < 0) {
		const char *e = getenv("NANPA2_DIFF_POISON");
		poison_on = e && *e && strcmp(e, "0");
		if (poison_on)
			fprintf(stderr, "diff: poisoning the scratch registers of every unit\n");
	}
	if (!poison_on)
		return;
	n++;
	for (int r = 0; r < 8; r++)
		if (u->scratch_regs & REG_BIT(r))
			cpu.r[r] = 0xdead0000u ^ (n << 4) ^ (uint32_t)r;
	if (!u->flags_out)
		cpu.cf = cpu.zf = cpu.sf = cpu.of = cpu.pf = n & 1;
}

/* Run the lifted original of u from the current CPU: a called unit returns
 * at the sentinel pushed as its return address; a jump unit is stopped at
 * its exit (an exit pc at the entry ESP, or a return above it), and the
 * result is that pc. */
static uint32_t oracle(const struct game_unit *u)
{
	if (!u->jump) {
		x86_exec(X86_ORACLE_TAG | u->addr); /* returns at the sentinel */
		return 0;
	}
	stop_tables();
	x86_stop_esp = cpu.r[ESP] + 4u * (uint32_t)u->stack_words;
	x86_stop_exits = game_vm_exits;
	x86_stop_units = stop_units;
	x86_stop_calls = stop_calls;
	x86_stop_started = 0;
	x86_stop_pc = 0;
	x86_exec(X86_ORACLE_TAG | u->addr);
	if (x86_stop_esp)
		x86_trap(u->addr, "diff: the oracle of a jump unit returned without an exit");
	return x86_stop_pc;
}

static uint32_t compare(const struct game_unit *u)
{
	static struct trace_set orig, mine;
	static struct diff_record rec;
	struct unit_stats *s = &stats[u - game_units];
	s->calls++;

	/* the state on entry, as the original sees it (for a called unit the
	 * return address on top) */
	if (!u->jump)
		push32(X86_RET_SENTINEL);
	struct cpu in = cpu;
	int countdown = x86_poll_countdown;
	x86_poll_countdown = INT_MAX; /* no timer ticks or presents in between */
	uint64_t icount0 = x86_icount;

	trace_begin();
	mode = MODE_ORACLE;
	uint32_t want_pc = oracle(u);
	mode = MODE_OFF;
	trace_end();
	struct cpu want = cpu;
	uint64_t icount1 = x86_icount;
	trace_set_clear(&orig);
	trace_collect(&orig);
	diff_record_from_trace(&rec, u->addr, &in, &want, &orig);
	rec.out_pc = want_pc;

	/* undo the original's writes, run the reimplementation */
	for (size_t i = 0; i < orig.n; i++)
		if (orig.b[i].state & TRACE_WRITTEN)
			wr8(orig.b[i].addr, orig.b[i].pre);
	cpu = in;
	trace_begin();
	mode = MODE_REIMPL;
	uint32_t got_pc = 0;
	if (u->jump) {
		got_pc = u->jump();
	} else {
		pop32();
		u->run();
	}
	mode = MODE_OFF;
	trace_end();
	trace_set_clear(&mine);
	trace_collect(&mine);

	char why[512];
	if (diff_check(u, &rec, got_pc, &mine, why, sizeof why)) {
		s->mismatches++;
		if (s->mismatches <= 20)
			fprintf(stderr, "diff MISMATCH %08x %s call %llu (esi %08x frame %u):%s\n", u->addr,
			        u->name, (unsigned long long)s->calls, in.r[ESI], rd32(0x446292), why);
		/* go on with the original's result */
		for (size_t i = 0; i < mine.n; i++)
			if (mine.b[i].state & TRACE_WRITTEN)
				wr8(mine.b[i].addr, mine.b[i].pre);
		for (uint32_t i = 0; i < rec.n_out; i++)
			wr8(rec.out[i].addr, rec.out[i].value);
		cpu = want;
		got_pc = want_pc;
	}
	s->oracle_insns += icount1 - icount0;
	s->reimpl_insns += x86_icount - icount1;
	x86_icount = icount1; /* charge the original's instruction count */
	x86_poll_countdown = countdown;
	record(u, s, &rec);
	return got_pc;
}

int game_call(uint32_t addr)
{
	const struct game_unit *u = game_unit_find(addr);
	if (!u || !u->run)
		return 0;
	if (diff_on < 0)
		diff_init();
	if (mode == MODE_ORACLE) {
		push32(X86_RET_SENTINEL);
		x86_exec(X86_ORACLE_TAG | addr);
		return 1;
	}
	if (diff_on && mode == MODE_OFF)
		compare(u);
	else
		u->run();
	if (mode == MODE_OFF)
		poison(u);
	return 1;
}

int game_jump(uint32_t addr, uint32_t *next)
{
	const struct game_unit *u = game_unit_find(addr);
	if (!u || !u->jump)
		return 0;
	if (diff_on < 0)
		diff_init();
	if (mode == MODE_ORACLE || (u->native && u->native())) {
		/* inside an original, or a case the C handler leaves to the
		 * native code: the lifted handler, inline */
		*next = X86_ORACLE_TAG | addr;
		if (mode == MODE_OFF && stats)
			stats[u - game_units].native_runs++;
		return 1;
	}
	if (diff_on && mode == MODE_OFF)
		*next = compare(u);
	else
		*next = u->jump();
	if (mode == MODE_OFF)
		poison(u);
	return 1;
}
