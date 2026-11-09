/* Deterministic mode, recording and replay: see replay.h. */
#include "replay.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock.h"
#include "game/pace.h"
#include "game/state_view.h"
#include "hle.h"

#define WM_QUIT_MSG 0x12u
#define PAGE 4096u

static int det_state = -1; /* -1 not decided yet */
static int recording, replaying;
static FILE *rec;
static uint64_t cp_ticks = 250;
static uint32_t cur_tick, cur_k;
static uint64_t last_cp_idx;
static char save_root[1024];
/* NANPA2_REPLAY_CHECK=li[,raw.clock,raw.data,...]: compare only the keys that start
 * with one of the comma-separated prefixes ("li" = every li.* key); unset = all keys;
 * "none" = replay the messages without any checkpoint comparison */
static const char *check_keys;
static int key_checked(const char *tok, size_t kl)
{
	if (!check_keys)
		return 1;
	for (const char *p = check_keys; *p;) {
		size_t n = strcspn(p, ",");
		if (n == 2 && !strncmp(p, "li", 2) ? !strncmp(tok, "li.", 3) : (n <= kl && !strncmp(tok, p, n)))
			return 1;
		p += n + (p[n] == ',');
	}
	return 0;
}

/* what the run has reached (the layout independent part) */
static uint64_t files_hash, mes_hash, nfiles, nmes;
static uint64_t vm_ops, vm_last;

/* replay input: events and checkpoints in file order */
struct rev {
	uint32_t tick, k, msg, wparam, lparam;
};
struct rcp {
	uint32_t tick, k;
	int fin;
	char *text;
};
static struct rev *revs;
static size_t nrevs, next_rev;
static struct rcp *rcps;
static size_t nrcps, next_rcp;
static int have_fin, quit_sent;
static char golden_saves[512];

int replay_det(void)
{
	if (det_state < 0) {
		const char *d = getenv("NANPA2_DETERMINISTIC");
		det_state = (d && atoi(d)) || getenv("NANPA2_RECORD") || getenv("NANPA2_REPLAY");
	}
	return det_state;
}

int replay_replaying(void)
{
	return replaying;
}

uint64_t replay_ms(void)
{
	return clock_us() / 1000;
}

/* ---- hashing: 8 bytes a step, multiply and rotate ---- */

uint64_t replay_hash(const void *p, size_t n, uint64_t h)
{
	const uint8_t *b = p;
	h ^= 0x9E3779B97F4A7C15ull + n;
	while (n >= 8) {
		uint64_t v;
		memcpy(&v, b, 8);
		h = (h ^ (v * 0xC2B2AE3D27D4EB4Full));
		h = ((h << 31) | (h >> 33)) * 0x9E3779B185EBCA87ull;
		b += 8;
		n -= 8;
	}
	uint64_t v = 0;
	memcpy(&v, b, n);
	h = (h ^ (v * 0xC2B2AE3D27D4EB4Full));
	h = ((h << 31) | (h >> 33)) * 0x9E3779B185EBCA87ull;
	h ^= h >> 29;
	return h;
}

static uint64_t hash_str(const char *s, uint64_t h)
{
	return replay_hash(s, strlen(s), h);
}

/* ---- what the run has reached ---- */

static void base_upper(const char *name, char *out, size_t cap)
{
	const char *b = name;
	for (const char *q = name; *q; q++)
		if (*q == '\\' || *q == '/' || *q == ':')
			b = q + 1;
	size_t i = 0;
	for (; b[i] && i + 1 < cap; i++)
		out[i] = (char)toupper((unsigned char)b[i]);
	out[i] = 0;
}

void replay_note_file(const char *name)
{
	if (!replay_det())
		return;
	char up[64];
	base_upper(name, up, sizeof up);
	files_hash = hash_str(up, files_hash);
	nfiles++;
	size_t n = strlen(up);
	if (n > 4 && !strcmp(up + n - 4, ".MES")) {
		mes_hash = hash_str(up, mes_hash);
		nmes++;
	}
}

void replay_note_vm(uint32_t off)
{
	vm_ops++;
	vm_last = off;
}

/* FLAG0-9 of the save folder: names and contents (the game folder's
 * initial FLAG files are not the player's saves and stay out) */
static uint64_t saves_hash(int *count)
{
	uint64_t h = 0;
	int n = 0;
	for (int i = 0; i < 10; i++) {
		char path[1200];
		snprintf(path, sizeof path, "%s/flag%d", save_root, i);
		FILE *f = fopen(path, "rb");
		if (!f)
			continue;
		static uint8_t buf[1 << 16];
		size_t len = fread(buf, 1, sizeof buf, f);
		fclose(f);
		h = replay_hash(buf, len, h + (uint64_t)i * 0x100000001B3ull);
		n++;
	}
	if (count)
		*count = n;
	return h;
}

/* ---- the checkpoint ---- */

#define DATA_LO 0x431000u
#define DATA_HI 0x4c0000u

static int build_cp(char *out, size_t cap)
{
	size_t n = 0;
#define ADD(...) (n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, __VA_ARGS__))
	static uint8_t frame[640 * 480];
	static uint32_t pal[256];
	uint64_t hs = 0, hp = 0;
	if (ddraw_snapshot(frame, pal)) {
		hs = replay_hash(frame, sizeof frame, 0);
		hp = replay_hash(pal, sizeof pal, 0);
	}
	int nsaves;
	uint64_t sv = saves_hash(&nsaves);
	uint8_t img[STATE_SAVE_SIZE];
	state_save_image(img);
	ADD("li.frame=%" PRIx32, state_frame());
	ADD(" li.screen=%016" PRIx64, hs);
	ADD(" li.pal=%016" PRIx64, hp);
	ADD(" li.save=%016" PRIx64, replay_hash(img, sizeof img, 0));
	ADD(" li.mes=%d/%016" PRIx64, (int)nmes, mes_hash);
	ADD(" li.files=%d/%016" PRIx64, (int)nfiles, files_hash);
	ADD(" li.vm=%" PRIu64 "/%" PRIx64, vm_ops, vm_last);
	ADD(" li.flags=%d/%016" PRIx64, nsaves, sv);
	ADD(" raw.clock=%" PRIu64, (uint64_t)clock_us());
	ADD(" raw.gdata=%016" PRIx64, state_data_hash());
	ADD(" raw.cpu=%016" PRIx64, replay_hash(&cpu.r[0], sizeof cpu.r, 0));
	static uint8_t data[DATA_HI - DATA_LO];
	x86_read_virtual(DATA_LO, data, sizeof data);
	ADD(" raw.data=%016" PRIx64, replay_hash(data, sizeof data, 0));
	uint32_t heap = x86_heap_top();
	ADD(" raw.heap=%016" PRIx64, replay_hash(x86_ptr(HEAP_BASE, heap - HEAP_BASE), heap - HEAP_BASE, 0));
	ADD(" raw.stack=%016" PRIx64, replay_hash(x86_ptr(cpu.r[ESP], STACK_TOP - cpu.r[ESP]), STACK_TOP - cpu.r[ESP], 0));
	ADD(" raw.pg=");
	for (uint32_t a = 0; a < sizeof data; a += PAGE) {
		uint32_t len = sizeof data - a < PAGE ? (uint32_t)sizeof data - a : PAGE;
		ADD("%04x", (unsigned)(replay_hash(data + a, len, 0) & 0xffff));
	}
#undef ADD
	return (int)n;
}

/* the keys of a checkpoint line that differ: appended to out */
static int diff_cp(const char *golden, const char *live, char *out, size_t cap)
{
	int ndiff = 0;
	size_t o = 0;
	char *g = strdup(golden);
	for (char *tok = strtok(g, " "); tok; tok = strtok(NULL, " ")) {
		char *eq = strchr(tok, '=');
		if (!eq)
			continue;
		size_t kl = (size_t)(eq - tok);
		if (!key_checked(tok, kl))
			continue;
		/* the same key in the live line */
		const char *p = live;
		const char *lv = NULL;
		while (*p) {
			if (!strncmp(p, tok, kl) && p[kl] == '=' && (p == live || p[-1] == ' ')) {
				lv = p + kl + 1;
				break;
			}
			const char *sp = strchr(p, ' ');
			if (!sp)
				break;
			p = sp + 1;
		}
		size_t vl = strlen(eq + 1);
		size_t ll = lv ? strcspn(lv, " ") : 0;
		if (lv && ll == vl && !strncmp(lv, eq + 1, vl))
			continue;
		ndiff++;
		if (!strcmp(tok, "raw.pg") || !strncmp(tok, "raw.pg=", 7)) {
			o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, " raw.pg:");
			int shown = 0;
			for (uint32_t i = 0; lv && i * 4 < vl && i * 4 < ll; i++)
				if (strncmp(eq + 1 + i * 4, lv + i * 4, 4) && shown++ < 8)
					o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, " %06x",
							      DATA_LO + i * PAGE);
		} else {
			o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, " %.*s golden=%s live=%.*s", (int)kl, tok,
					      eq + 1, (int)ll, lv ? lv : "-");
		}
	}
	free(g);
	return ndiff;
}

static void checkpoint(int fin)
{
	static char text[4096];
	build_cp(text, sizeof text);
	if (rec) {
		fprintf(rec, "%s %u %u %s\n", fin ? "fin" : "cp", cur_tick, cur_k, text);
		fflush(rec);
	}
	if (!replaying)
		return;
	if (check_keys && !strcmp(check_keys, "none"))
		return; /* messages only: recorded input on a build with other timing */
	size_t at = next_rcp;
	if (at >= nrcps || rcps[at].fin != fin) {
		/* the recording has no checkpoint here (a longer run than it) */
		if (fin && !have_fin)
			return;
		fprintf(stderr, "replay: no recorded checkpoint %zu for %s at tick %u pump %u\n", at,
			fin ? "the final one" : "this", cur_tick, cur_k);
		fflush(stderr);
		exit(3);
	}
	next_rcp++;
	static char d[4096];
	d[0] = 0;
	int nd = diff_cp(rcps[at].text, text, d, sizeof d);
	if (rcps[at].tick != cur_tick || rcps[at].k != cur_k) {
		fprintf(stderr, "replay: checkpoint %zu taken at tick %u pump %u, recorded at tick %u pump %u\n", at,
			cur_tick, cur_k, rcps[at].tick, rcps[at].k);
		nd++;
	}
	if (nd) {
		fprintf(stderr, "replay: DIVERGED at checkpoint %zu (tick %u pump %u):%s\n", at, cur_tick, cur_k, d);
		fflush(stderr);
		exit(3);
	}
}

/* ---- pump hook ---- */

void replay_pump(void)
{
	uint32_t t = win32_timer_ticks();
	if (t != cur_tick) {
		cur_tick = t;
		cur_k = 0;
	} else {
		cur_k++;
	}
	if (cp_ticks && (recording || replaying) && cur_tick / cp_ticks > last_cp_idx) {
		last_cp_idx = cur_tick / cp_ticks;
		checkpoint(0);
	}
	if (replaying) {
		while (next_rev < nrevs &&
		       (revs[next_rev].tick < cur_tick || (revs[next_rev].tick == cur_tick && revs[next_rev].k <= cur_k))) {
			struct rev *e = &revs[next_rev++];
			if (rec) /* --replay with --record: the same messages, to carry the recording over to a new checkpoint format */
				fprintf(rec, "ev %u %u %x %x %x\n", cur_tick, cur_k, e->msg, e->wparam, e->lparam);
			win32_post_message(e->msg, e->wparam, e->lparam);
		}
		if (!have_fin && next_rev >= nrevs && next_rcp >= nrcps && !quit_sent) {
			quit_sent = 1;
			win32_post_message(WM_QUIT_MSG, 0, 0);
		}
	}
}

int replay_external(uint32_t msg, uint32_t wparam, uint32_t lparam, int from_replay)
{
	(void)from_replay;
	if (replaying)
		return msg == WM_QUIT_MSG; /* only the host's close request */
	if (rec) {
		fprintf(rec, "ev %u %u %x %x %x\n", cur_tick, cur_k, msg, wparam, lparam);
		fflush(rec);
	}
	return 1;
}

/* ---- files ---- */

static void saves_line(char *out, size_t cap)
{
	int n;
	uint64_t h = saves_hash(&n);
	snprintf(out, cap, "%d/%016" PRIx64, n, h);
}

static int load_replay(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "replay: cannot open %s\n", path);
		return 0;
	}
	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	int ok = 0, pace_ok = 0;
	while ((len = getline(&line, &cap, f)) > 0) {
		while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = 0;
		uint32_t t, k, m, w, l;
		if (!strncmp(line, "nanpa2-replay ", 14)) {
			ok = atoi(line + 14) == 2;
		} else if (!strncmp(line, "pace ", 5)) {
			pace_ok = atoi(line + 5) == PACE_VERSION;
		} else if (!strncmp(line, "cp_ticks ", 9)) {
			cp_ticks = strtoull(line + 9, NULL, 10);
		} else if (!strncmp(line, "saves ", 6)) {
			snprintf(golden_saves, sizeof golden_saves, "%s", line + 6);
		} else if (sscanf(line, "ev %u %u %x %x %x", &t, &k, &m, &w, &l) == 5) {
			revs = realloc(revs, (nrevs + 1) * sizeof *revs);
			revs[nrevs++] = (struct rev){ t, k, m, w, l };
		} else if (!strncmp(line, "cp ", 3) || !strncmp(line, "fin ", 4)) {
			int fin = line[0] == 'f';
			int off = 0;
			if (sscanf(line + (fin ? 4 : 3), "%u %u %n", &t, &k, &off) >= 2 && off) {
				rcps = realloc(rcps, (nrcps + 1) * sizeof *rcps);
				rcps[nrcps++] = (struct rcp){ t, k, fin, strdup(line + (fin ? 4 : 3) + off) };
				if (fin)
					have_fin = 1;
			}
		}
	}
	free(line);
	fclose(f);
	ok = ok && pace_ok;
	if (!ok)
		fprintf(stderr, "replay: %s is not a recording of this version (format 2, pace %d)\n", path, PACE_VERSION);
	return ok;
}

void replay_init(const char *save_dir)
{
	snprintf(save_root, sizeof save_root, "%s", save_dir);
	if (!replay_det())
		return;
	const char *c = getenv("NANPA2_CP_TICKS");
	if (c && atoi(c) > 0)
		cp_ticks = (uint64_t)atoi(c);
	const char *chk = getenv("NANPA2_REPLAY_CHECK");
	check_keys = chk && *chk ? strdup(chk) : NULL;
	const char *rp = getenv("NANPA2_REPLAY");
	const char *rc = getenv("NANPA2_RECORD");
	if (rp) {
		if (!load_replay(rp))
			exit(2);
		char now[64];
		saves_line(now, sizeof now);
		if (strcmp(now, golden_saves)) {
			fprintf(stderr, "replay: the save folder is not as it was recorded (recorded %s, now %s)\n",
				golden_saves, now);
			exit(2);
		}
		replaying = 1;
		fprintf(stderr, "replay: %s: %zu messages, %zu checkpoints every %u ticks%s%s%s\n", rp, nrevs, nrcps,
			(unsigned)cp_ticks, check_keys ? " (keys: " : "", check_keys ? check_keys : "", check_keys ? ")" : "");
	}
	if (rc) {
		rec = fopen(rc, "wb");
		if (!rec) {
			fprintf(stderr, "record: cannot write %s\n", rc);
			exit(2);
		}
		char now[64];
		saves_line(now, sizeof now);
		fprintf(rec, "nanpa2-replay 2\npace %d\ncp_ticks %u\nsaves %s\n", PACE_VERSION, (unsigned)cp_ticks,
			now);
		fflush(rec);
		recording = 1;
	}
}

void replay_finish(void)
{
	if (!recording && !replaying)
		return;
	if (replaying && next_rcp < nrcps && !rcps[next_rcp].fin) {
		fprintf(stderr, "replay: ended at tick %u with %zu recorded checkpoints unused\n", cur_tick,
			nrcps - next_rcp);
		exit(3);
	}
	checkpoint(1);
	if (replaying && next_rcp < nrcps) {
		fprintf(stderr, "replay: ended at tick %u with %zu recorded checkpoints unused\n", cur_tick,
			nrcps - next_rcp);
		exit(3);
	}
	if (replaying)
		fprintf(stderr, "replay: ok, %zu checkpoints matched\n", next_rcp);
	if (rec)
		fclose(rec);
	rec = NULL;
	recording = replaying = 0;
}
