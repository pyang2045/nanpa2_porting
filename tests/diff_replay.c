/*
 * Differential replay: runs each reimplemented unit (src/game) on call
 * records of its lifted original (made by NANPA2_DIFF_DIR, src/game/game.c)
 * and compares registers and written memory.
 *
 * usage: diff_replay DIR...    exit 0 all equal, 1 differences, 77 no records
 *
 * Only the record's inputs are in memory (everything else is zero), and a
 * read of any other byte counts as a difference, so this also shows that a
 * unit depends on nothing but emulated memory: no lifted code, no HLE.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/diff.h"
#include "game/expr.h"
#include "game/game.h"
#include "game/trace.h"
#include "x86/x86.h"

/* x86_call (src/x86/x86.c) needs it; units never call lifted code */
void x86_exec(uint32_t pc)
{
	x86_trap(pc, "diff_replay: a unit called lifted code");
}

/* opcode handlers poll the host at their backward jumps; there is none */
void x86_poll(void)
{
	x86_poll_countdown = 1 << 30;
}

/* the hit test (04 23) fires the hotspot probe for the exploration bot */
void x86_probe(uint32_t pc)
{
	(void)pc;
}

struct counts {
	unsigned long records, mismatches;
};

static int replay_file(const char *path, struct counts *total, struct counts *per)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	char magic[sizeof DIFF_MAGIC - 1];
	int version = 0;
	if (fread(magic, 1, sizeof magic, f) == sizeof magic)
		version = !memcmp(magic, DIFF_MAGIC, sizeof magic)      ? 2
		          : !memcmp(magic, DIFF_MAGIC_V1, sizeof magic) ? 1
		                                                        : 0;
	if (!version) {
		fprintf(stderr, "%s: not a record file\n", path);
		fclose(f);
		total->mismatches++;
		return 1;
	}
	static struct diff_record rec;
	static struct trace_set got;
	int r;
	while ((r = diff_read(f, &rec, version)) == 1) {
		const struct game_unit *u = game_unit_find(rec.unit);
		total->records++;
		if (!u) {
			fprintf(stderr, "%s: record of %08x, which is not a unit\n", path, rec.unit);
			total->mismatches++;
			continue;
		}
		struct counts *c = &per[u - game_units];
		c->records++;
		for (uint32_t i = 0; i < rec.n_in; i++)
			wr8(rec.in[i].addr, rec.in[i].value);
		diff_set_cpu(rec.in_r, rec.in_flags);
		trace_begin();
		uint32_t pc = 0;
		if (u->jump) {
			pc = u->jump();
		} else {
			pop32(); /* the return address, as hle_call does */
			u->run();
		}
		trace_end();
		trace_set_clear(&got);
		trace_collect(&got);
		char why[512];
		if (diff_check(u, &rec, pc, &got, why, sizeof why)) {
			if (c->mismatches < 20)
				fprintf(stderr, "MISMATCH %08x %s record %lu (esi %08x):%s\n", u->addr, u->name,
				        c->records, rec.in_r[ESI], why);
			c->mismatches++;
			total->mismatches++;
		}
		/* back to all zero */
		for (uint32_t i = 0; i < rec.n_in; i++)
			wr8(rec.in[i].addr, 0);
		for (size_t i = 0; i < got.n; i++)
			wr8(got.b[i].addr, 0);
	}
	if (r < 0) {
		fprintf(stderr, "%s: truncated record (ignored)\n", path);
	}
	fclose(f);
	return 1;
}

int main(int argc, char **argv)
{
	x86_mem = calloc(1, MEM_END - MEM_BASE);
	if (!x86_mem)
		return 2;
	struct counts total = { 0, 0 };
	struct counts *per = calloc((size_t)game_unit_count, sizeof *per);
	int files = 0;
	for (int a = 1; a < argc; a++) {
		DIR *d = opendir(argv[a]);
		if (!d)
			continue;
		struct dirent *e;
		while ((e = readdir(d))) {
			size_t n = strlen(e->d_name);
			if (n < 7 || strcmp(e->d_name + n - 7, ".n2diff"))
				continue;
			char path[2048];
			snprintf(path, sizeof path, "%s/%s", argv[a], e->d_name);
			files += replay_file(path, &total, per);
		}
		closedir(d);
	}
	for (int i = 0; i < game_unit_count; i++)
		printf("%08x %-24s records %8lu mismatches %lu\n", game_units[i].addr, game_units[i].name,
		       per[i].records, per[i].mismatches);
	printf("script_eval tokens:");
	for (int t = 0; t < 256; t++)
		if (script_eval_coverage[t])
			printf(" %02x:%llu", t, (unsigned long long)script_eval_coverage[t]);
	printf("\n");
	if (!total.records) {
		printf("diff_replay: no records (make some with NANPA2_DIFF_DIR, docs/reimpl.md)\n");
		return 77;
	}
	printf("diff_replay: %lu records in %d files, %lu mismatches\n", total.records, files,
	       total.mismatches);
	return total.mismatches ? 1 : 0;
}
