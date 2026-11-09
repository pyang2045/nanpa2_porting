/* Exploration bot (NANPA2_EXPLORE=seed): plays the game on its own so that
 * long headless runs reach many scenes and expose engine bugs.
 *
 * It watches the script VM (opcode fetch 0x40616e), the hit test (ext op
 * 0x23, 0x4071a4) and file opens, and every few dozen game frames picks an
 * action for what the game is waiting on:
 *   menu   (ext op 0x13, 0x406671)  click a visible item: inline pop-ups
 *          (`04 13 01 item 02 item ... 00`) are listed at the top left of
 *          the picture, labelled menus (`04 13 L`, defined at `12 L 03 02 01
 *          ...`, label table 0x445dbc) in the text box grid; item guards
 *          (`02 0f expr 03 text`) are evaluated with the game's own
 *          expression code (0x4037fc), and items reading 結束 are skipped
 *   text   (op 0x11, wait for a click)  click
 *   scene  (hit test active)  click a hotspot rectangle, sometimes
 *          right-click for the scene's pop-up menu
 *   town   (`04 26 3c`, 0x4051c2)  hold the button towards a point (walk)
 *          or click near the player (enter a place)
 *   other  (title, pickers, animations)  click somewhere likely
 * It logs `explore: reached NAME frame F` the first time each MES file
 * loads and `explore: stall ...` when no new script offset has run for a
 * long time; NANPA2_EXPLORE_FRAMES ends the run (WM_QUIT) after that many
 * frames; NANPA2_EXPLORE_AFTER=F keeps it idle until frame F (so that
 * NANPA2_INPUT can first load a save); NANPA2_EXPLORE_LOG=1 logs menus and
 * clicks. Runs are repeatable only roughly (the game timer is host-paced):
 * the seed and frame numbers locate a problem, they do not replay it. */
#include "explore.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/expr.h"
#include "game/state.h"
#include "hle.h"

#define FRAME() RT(frame)
#define SCRIPT 0x435e78u
#define WM_MOUSEMOVE 0x200
#define WM_LBUTTONDOWN 0x201
#define WM_LBUTTONUP 0x202
#define WM_RBUTTONDOWN 0x204
#define WM_RBUTTONUP 0x205

static int on, verbose;
static uint64_t rng;
static uint32_t end_frame, start_frame, stall_frames = 40000;

static uint32_t rnd(uint32_t n)
{
	rng = rng * 6364136223846793005ull + 1442695040888963407ull;
	return n ? (uint32_t)(rng >> 33) % n : 0;
}

/* ---- what the game is doing */
enum { OP_OTHER, OP_MENU, OP_TEXT };
static int last_op;
static uint32_t last_off, last_fetch_frame, town_frame, hit_frame;
static char mes[32];

#define MAX_ITEMS 16
static struct item {
	int x, y;
} items[MAX_ITEMS];
static int nitems;
static uint32_t menu_off, menu_next, menu_frame;
static int menu_active;

#define MAX_HITS 100
static struct hit {
	int x0, y0, x1, y1;
} hits[MAX_HITS];
static int nhits, hits_file = -1;

/* ---- coverage and novelty: one bit per script offset per MES file */
#define MAX_MES 512
static struct {
	char name[32];
	uint8_t *seen;
} files[MAX_MES];
static int nfiles, cur_file = -1;
static uint32_t novel_frame, stall_reported;

static int file_index(const char *name)
{
	for (int i = 0; i < nfiles; i++)
		if (!strcmp(files[i].name, name))
			return i;
	if (nfiles == MAX_MES)
		return -1;
	snprintf(files[nfiles].name, sizeof files[nfiles].name, "%s", name);
	files[nfiles].seen = calloc(65536 / 8, 1);
	return nfiles++;
}

/* ---- queued input */
#define MAX_Q 64
static struct qev {
	uint32_t frame, msg, wp, lp;
} q[MAX_Q];
static int nq;
static uint32_t next_decide;

static void push(uint32_t at, uint32_t msg, uint32_t wp, int x, int y)
{
	if (nq < MAX_Q)
		q[nq++] = (struct qev){ at, msg, wp, ((uint32_t)y << 16) | (uint32_t)x };
}

static void click(uint32_t t, int x, int y)
{
	if (verbose)
		fprintf(stderr, "explore: frame %u %s click %d,%d\n", t, mes, x, y);
	push(t, WM_MOUSEMOVE, 0, x, y);
	push(t + 8, WM_MOUSEMOVE, 0, x, y);
	push(t + 12, WM_LBUTTONDOWN, 1, x, y);
	push(t + 20, WM_LBUTTONUP, 0, x, y);
}

static void on_trap(void)
{
	fprintf(stderr, "explore: trap frame %u mes %s (last fetch %04x op %02x)\n", FRAME(), mes,
		last_off, rd8(SCRIPT + last_off));
}

void explore_init(void)
{
	const char *s = getenv("NANPA2_EXPLORE");
	if (!s)
		return;
	on = 1;
	x86_trap_hook = on_trap;
	rng = strtoull(s, NULL, 0) * 2654435761u + 1;
	const char *e = getenv("NANPA2_EXPLORE_FRAMES");
	if (e)
		end_frame = (uint32_t)strtoul(e, NULL, 0);
	e = getenv("NANPA2_EXPLORE_AFTER"); /* idle until then (NANPA2_INPUT can load a save) */
	if (e)
		start_frame = (uint32_t)strtoul(e, NULL, 0);
	e = getenv("NANPA2_EXPLORE_STALL");
	if (e)
		stall_frames = (uint32_t)strtoul(e, NULL, 0);
	verbose = getenv("NANPA2_EXPLORE_LOG") != NULL;
	fprintf(stderr, "explore: seed %s\n", s);
}

int explore_on(void)
{
	return on;
}

void explore_file(const char *name)
{
	if (!on)
		return;
	const char *b = strrchr(name, '\\');
	b = b ? b + 1 : name;
	size_t n = strlen(b);
	if (n < 5 || SDL_strcasecmp(b + n - 4, ".mes"))
		return;
	char low[32];
	snprintf(low, sizeof low, "%s", b);
	for (char *p = low; *p; p++)
		if (*p >= 'A' && *p <= 'Z')
			*p += 32;
	int had = nfiles;
	menu_active = 0;
	cur_file = file_index(low);
	snprintf(mes, sizeof mes, "%s", low);
	/* flags 2010 (diary-save marker) and 2000 (40.MES right-click gate):
	 * nibbles at 0x444e98, odd flags in the high nibble */
	uint8_t f2010 = rd8(0x444e98 + 1005) & 15, f2000 = rd8(0x444e98 + 1000) & 15;
	if (nfiles > had)
		fprintf(stderr, "explore: reached %s frame %u f2010 %u f2000 %u\n", low, FRAME(), f2010, f2000);
}

/* ---- menus */

/* Skip an expression (up to and including its 0x03). */
static uint32_t skip_expr(uint32_t p)
{
	for (int n = 0; n < 64; n++) {
		uint8_t c = rd8(SCRIPT + p++);
		if (c == 0x03)
			return p;
		if (c == 0x07)
			p += 1;
		else if (c == 0x08)
			p += 2;
		else if (c == 0x09)
			p += 3;
	}
	return p;
}

/* Value of the expression at script offset p, by the game's evaluator
 * (script_eval, 0x4037fc; the random operator advances the seed at
 * 0x4313c4, which is put back). */
static uint32_t eval(uint32_t p)
{
	uint16_t seed = AN(random_seed);
	/* the bot's own work is not the game's: it calls the evaluator directly, which
	 * spends no game time (only vm_eval charges), or a replay, which has no bot, would
	 * run on another clock than the recording */
	int bare;
	uint32_t v = script_eval(&p, &bare) & 0xffff;
	AN_SET(random_seed, seed);
	return v;
}

static int contains(uint32_t a, uint32_t b, const char *s)
{
	size_t n = strlen(s);
	for (uint32_t p = a; p + n <= b; p++)
		if (!memcmp(x86_ptr(SCRIPT + p, (uint32_t)n), s, n))
			return 1;
	return 0;
}

/* `04 13 arg`: list the visible items and where they are drawn. */
static void menu_seen(uint32_t off)
{
	uint32_t p = off + 2, list;
	int popup = rd8(SCRIPT + p) == 0x01;
	/* the op after this one: the menu is up until the VM gets there (while
	 * it waits, the menu code runs script handlers, 0x4061cd) */
	if (popup) {
		menu_next = p + 1;
		while (rd8(SCRIPT + menu_next) != 0x00 && menu_next - p < 1024)
			menu_next++;
		menu_next++;
	} else {
		menu_next = skip_expr(p);
	}
	if (popup) {
		list = p + 1;
	} else {
		uint32_t label = eval(p);
		list = rd16(0x445dbc + 2 * (label & 0xff));
		if (rd8(SCRIPT + list) == 0x12)
			list = skip_expr(list + 1);
		for (int k = 0; k < 4 && rd8(SCRIPT + list) != 0x01; k++)
			list++;
		list++;
	}
	nitems = 0;
	int shown = 0;
	for (int guard = 0; guard < 64; guard++) {
		uint32_t start = list;
		int visible = 1;
		if (rd8(SCRIPT + list) == 0x0f) {
			visible = eval(list + 1) != 0;
			list = skip_expr(list + 1);
		}
		uint32_t text = list;
		while (rd8(SCRIPT + list) != 0x02 && rd8(SCRIPT + list) != 0x00 && list - start < 256)
			list++;
		if (visible) {
			/* 結束 (end the game) in Big5 */
			int quit = contains(text, list, "\xb5\xb2\xa7\xf4");
			if (!quit && nitems < MAX_ITEMS) {
				if (popup && !strcmp(mes, "start1.mes")) /* title: big buttons */
					items[nitems] = (struct item){ 320, 170 + 42 * shown };
				else if (popup)
					items[nitems] = (struct item){ 150, 57 + 18 * shown };
				else
					items[nitems] = (struct item){ shown % 2 ? 420 : 215, 383 + 24 * (shown / 2) };
				nitems++;
			}
			shown++;
		}
		if (rd8(SCRIPT + list) != 0x02)
			break;
		list++;
	}
	menu_off = off;
	if (verbose) {
		fprintf(stderr, "explore: menu at %04x %s, %d choices:", off, popup ? "pop-up" : "grid", nitems);
		for (int i = 0; i < nitems; i++)
			fprintf(stderr, " (%d,%d)", items[i].x, items[i].y);
		fprintf(stderr, "\n");
	}
}

void explore_vm(uint32_t off)
{
	if (!on)
		return;
	uint32_t f = FRAME();
	last_fetch_frame = f;
	last_off = off;
	uint8_t b0 = rd8(SCRIPT + off), b1 = rd8(SCRIPT + off + 1), b2 = rd8(SCRIPT + off + 2);
	if (b0 == 0x04 && b1 == 0x13) {
		last_op = OP_MENU;
		menu_active = 1;
		menu_frame = f;
		menu_seen(off);
	} else if (b0 == 0x11) {
		last_op = OP_TEXT;
		/* a menu left by a jump never fetches menu_next; a wait for a
		 * click means it is gone (35.MES 0x9df9 stalled seed 41) */
		menu_active = 0;
	} else {
		last_op = OP_OTHER;
	}
	if (off == menu_next)
		menu_active = 0;
	if (b0 == 0x04 && b1 == 0x26 && b2 == 0x3c)
		town_frame = f;
	if (cur_file >= 0) {
		uint8_t *s = files[cur_file].seen;
		if (!(s[off >> 3] & (1 << (off & 7)))) {
			s[off >> 3] |= (uint8_t)(1 << (off & 7));
			novel_frame = f;
		}
	}
}

void explore_hits(uint32_t table)
{
	if (!on)
		return;
	hit_frame = FRAME();
	hits_file = cur_file;
	nhits = 0;
	while (nhits < MAX_HITS && rd16(table + 10 * nhits) != 0xffff) {
		uint32_t e = table + 10 * nhits;
		hits[nhits] = (struct hit){ rd16(e + 2), rd16(e + 4) + 40, rd16(e + 6), rd16(e + 8) + 40 };
		nhits++;
	}
}

/* ---- decisions: at each place (a key built from the MES file, the script
 * offset and registers V and N) try every choice once before repeating */
#define MAX_KEYS 8192
static struct {
	uint32_t key, tried;
} keys[MAX_KEYS];

static uint32_t place(uint32_t extra)
{
	uint32_t h = 2166136261u;
	for (const char *p = mes; *p; p++)
		h = (h ^ (uint8_t)*p) * 16777619u;
	h = (h ^ extra) * 16777619u;
	h = (h ^ G.save.reg['V' - 0x40]) * 16777619u;
	return (h ^ G.save.reg['N' - 0x40]) * 16777619u | 1;
}

static uint32_t choose(uint32_t key, uint32_t n)
{
	if (n > 32)
		return rnd(n);
	uint32_t i = key % MAX_KEYS;
	while (keys[i].key && keys[i].key != key)
		i = (i + 1) % MAX_KEYS;
	keys[i].key = key;
	uint32_t all = n == 32 ? ~0u : (1u << n) - 1;
	if ((keys[i].tried & all) == all)
		keys[i].tried = 0;
	uint32_t c;
	do
		c = rnd(n);
	while (keys[i].tried & (1u << c));
	keys[i].tried |= 1u << c;
	return c;
}

static void decide(uint32_t f)
{
	uint32_t t = f + 2;
	if (menu_active && f - menu_frame <= 20) {
		/* let it draw; a click now would take the highlighted item */
		next_decide = menu_frame + 21;
		return;
	}
	/* a menu that ignores its items (NAME.MES 0xb23 shows a one-entry
	 * pop-up while the name grid takes the clicks): after a few tries,
	 * act as if it were not there half of the time */
	if (menu_active && (f - menu_frame < 400 || rnd(2))) {
		if (nitems && rnd(20)) {
			struct item *it = &items[choose(place(menu_off), (uint32_t)nitems)];
			click(t, it->x + (int)rnd(40) - 20, it->y);
		} else if (rnd(2)) {
			push(t, WM_RBUTTONDOWN, 2, 320, 250); /* cancel */
			push(t + 8, WM_RBUTTONUP, 0, 320, 250);
		} else {
			click(t, 320, 430);
		}
		next_decide = f + 40 + rnd(40);
		return;
	}
	if (last_op == OP_TEXT) {
		/* scenes with hotspots wait with op 0x11 too and hit-test the
		 * click afterwards (OPEN1.MES); a click outside every hotspot
		 * opens the scene menu there */
		if (nhits && hits_file == cur_file && rnd(100) < 70) {
			struct hit *h = &hits[choose(place(0x10000 + (uint32_t)nhits), (uint32_t)nhits)];
			click(t, (h->x0 + h->x1) / 2, (h->y0 + h->y1) / 2);
		} else {
			click(t, 320, 430);
		}
		next_decide = f + 15 + rnd(30);
		return;
	}
	if (f - hit_frame < 20 && nhits) {
		uint32_t r = rnd(100);
		if (r < 80) {
			struct hit *h = &hits[choose(place(0x10000 + (uint32_t)nhits), (uint32_t)nhits)];
			int x = (h->x0 + h->x1) / 2, y = (h->y0 + h->y1) / 2;
			push(t, WM_MOUSEMOVE, 0, x, y);
			click(t + 20, x, y);
		} else if (r < 92) {
			int x = 70 + (int)rnd(500), y = 50 + (int)rnd(270);
			push(t, WM_MOUSEMOVE, 0, x, y);
			push(t + 10, WM_RBUTTONDOWN, 2, x, y);
			push(t + 18, WM_RBUTTONUP, 0, x, y);
		} else {
			click(t, 70 + (int)rnd(500), 50 + (int)rnd(270));
		}
		next_decide = f + 40 + rnd(40);
		return;
	}
	if (f - town_frame < 20) {
		uint32_t r = rnd(100);
		if (r < 55) { /* walk */
			int x = 70 + (int)rnd(500), y = 45 + (int)rnd(280);
			uint32_t hold = 30 + rnd(150);
			push(t, WM_MOUSEMOVE, 0, x, y);
			push(t + 4, WM_LBUTTONDOWN, 1, x, y);
			push(t + hold, WM_LBUTTONUP, 0, x, y);
			next_decide = t + hold + 10;
		} else if (r < 95) { /* a place near the player (the map keeps him central) */
			click(t, 320 + (int)rnd(240) - 120, 190 + (int)rnd(160) - 80);
			next_decide = f + 40;
		} else {
			push(t, WM_RBUTTONDOWN, 2, 320, 200);
			push(t + 8, WM_RBUTTONUP, 0, 320, 200);
			next_decide = f + 40;
		}
		return;
	}
	/* title, pickers, animations: likely spots, or anywhere */
	static const struct item spots[] = {
		{ 320, 430 }, { 320, 170 }, { 320, 198 }, { 320, 226 }, { 320, 254 },
		{ 200, 170 }, { 248, 170 }, { 296, 170 }, { 344, 170 }, { 392, 170 }, { 440, 170 },
		{ 295, 82 }, { 340, 82 }, { 390, 82 }, { 438, 82 }, { 488, 82 }, { 533, 82 },
		{ 512, 288 }, { 512, 288 }, /* name entry (NAME.MES): END */
	};
	/* text menus drawn by script loops (OPEN.MES: `04 15 32` polls the mouse
	 * and the script finds the item) look like this too: click the text box
	 * grid as often as the text */
	uint32_t r = rnd(100);
	if (r < 35) {
		click(t, 320, 430);
	} else if (r < 70) {
		uint32_t k = choose(place(0x20000 + (last_off >> 6)), 6);
		click(t, k % 2 ? 420 : 215, 383 + 24 * (int)(k / 2));
	} else if (r < 88) {
		const struct item *sp = &spots[rnd(sizeof spots / sizeof spots[0])];
		click(t, sp->x, sp->y);
	} else {
		click(t, (int)rnd(640), (int)rnd(480));
	}
	next_decide = f + 30 + rnd(40);
}

void explore_tick(void)
{
	if (!on)
		return;
	uint32_t f = FRAME();
	for (int i = 0; i < nq;) {
		if ((int32_t)(f - q[i].frame) >= 0) {
			win32_post_ext(q[i].msg, q[i].wp, q[i].lp);
			q[i] = q[--nq];
		} else {
			i++;
		}
	}
	if (end_frame && f >= end_frame) {
		static int quit;
		if (!quit++) {
			fprintf(stderr, "explore: end frame %u mes %s\n", f, mes);
			win32_post_ext(0x12, 0, 0);
		}
		return;
	}
	if (novel_frame && f - novel_frame > stall_frames && stall_reported != novel_frame) {
		stall_reported = novel_frame;
		fprintf(stderr, "explore: stall frame %u mes %s: no new script offset since frame %u (last fetch %04x op %02x)\n",
			f, mes, novel_frame, last_off, rd8(SCRIPT + last_off));
	}
	if (nq || (int32_t)(f - next_decide) < 0 || f < start_frame)
		return;
	decide(f);
}
