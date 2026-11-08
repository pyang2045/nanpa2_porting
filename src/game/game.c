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
	uint64_t calls, mismatches, recorded, oracle_insns, reimpl_insns;
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
	if (!diff_dir || s->recorded >= diff_max)
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

static void run_oracle(uint32_t addr)
{
	push32(X86_RET_SENTINEL);
	x86_exec(X86_ORACLE_TAG | addr);
}

static void compare(const struct game_unit *u)
{
	static struct trace_set orig, mine;
	static struct diff_record rec;
	struct unit_stats *s = &stats[u - game_units];
	s->calls++;

	/* the state on entry, as the original sees it (return address on top) */
	push32(X86_RET_SENTINEL);
	struct cpu in = cpu;
	int countdown = x86_poll_countdown;
	x86_poll_countdown = INT_MAX; /* no timer ticks or presents in between */
	uint64_t icount0 = x86_icount;

	trace_begin();
	mode = MODE_ORACLE;
	x86_exec(X86_ORACLE_TAG | u->addr); /* returns at the sentinel */
	mode = MODE_OFF;
	trace_end();
	struct cpu want = cpu;
	uint64_t icount1 = x86_icount;
	trace_set_clear(&orig);
	trace_collect(&orig);
	diff_record_from_trace(&rec, u->addr, &in, &want, &orig);

	/* undo the original's writes, run the reimplementation */
	for (size_t i = 0; i < orig.n; i++)
		if (orig.b[i].state & TRACE_WRITTEN)
			wr8(orig.b[i].addr, orig.b[i].pre);
	cpu = in;
	trace_begin();
	mode = MODE_REIMPL;
	pop32();
	u->run();
	mode = MODE_OFF;
	trace_end();
	trace_set_clear(&mine);
	trace_collect(&mine);

	char why[512];
	if (diff_check(u, &rec, &mine, why, sizeof why)) {
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
	}
	s->oracle_insns += icount1 - icount0;
	s->reimpl_insns += x86_icount - icount1;
	x86_icount = icount1; /* charge the original's instruction count */
	x86_poll_countdown = countdown;
	record(u, s, &rec);
}

int game_call(uint32_t addr)
{
	const struct game_unit *u = game_unit_find(addr);
	if (!u)
		return 0;
	if (diff_on < 0)
		diff_init();
	if (mode == MODE_ORACLE)
		run_oracle(addr);
	else if (diff_on && mode == MODE_OFF)
		compare(u);
	else
		u->run();
	return 1;
}
