/* Developer hooks for scripted, headless testing (environment variables):
 *   NANPA2_SHOT_DIR=dir NANPA2_SHOT_SCHEDULE=start_ms,interval_ms,count
 *       save the screen as dir/shot_NNN.bmp on that schedule
 *   NANPA2_SHOT_PRESENT=1 (2: also dir/bitmap_NNN.bmp, the same frame without the text layer, and
 *       dir/prev_NNN.bmp, the raw frame the presenter was showing, which is what the capture shows)
 *       also save what the presenter draws (CRT included) at the window's
 *       drawable size (NANPA2_SHOT_SIZE=WxH: at that size) as dir/present_NNN.bmp
 *   NANPA2_INPUT="click,MS,X,Y;rclick,MS,X,Y;move,MS,X,Y;key,MS,VK;quit,MS"
 *       timed input in 640x480 screen coordinates, MS after start; clicks and
 *       keys are held for HOLD_MS
 *       down,MS,X,Y / up,MS,X,Y   press / release the left button (held)
 *       sync,0,NAME   later events count from when the game opens file NAME
 *                     (e.g. DATA\NAME.GP4; case-insensitive substring);
 *                     unfired earlier events are dropped
 *       sync,0,@HEX   same, when the script VM fetches the opcode at offset
 *                     HEX of the loaded script (0x40616e)
 *       shot,MS       save the next frame as dir/ev_NNN.bmp
 *   NANPA2_INPUT_CLOCK=frames  MS counts game time (16 ms ticks of 0x446292)
 *       instead of wall-clock time, for repeatable runs
 *   NANPA2_SPEED=N  run the game N times faster than the original (clock.c; 0 = as
 *       fast as the host can; NANPA2_TURBO is the old name), for long scripted runs
 *   NANPA2_HOTSPOTS=1  log the script hit-test table when it changes */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev/dev.h"
#include "dev/explore.h"
#include "game/file.h"
#include "game/gmem.h"
#include "game/state_view.h"
#include "platform/window.h"
#include "platform/present.h"
#include "platform/textlayer.h"
#include "dev/replay.h"
#include "dev/status.h"
#include "hooks.h"

static const char *shot_dir;
static uint32_t shot_start, shot_interval, shot_count, shot_done;
static uint32_t t0;

#define MAX_INPUT 8192
#define HOLD_MS 120
static struct ev {
	uint32_t ms;
	char kind[8];
	int x, y;
	int done;
	int seg;           /* events after the n-th sync count from it */
	char file[32];     /* sync: file name */
} input[MAX_INPUT];
static int ninput;
static int nseg;
static uint32_t seg_start[MAX_INPUT + 1]; /* 0 = not started (seg 0 at t0) */
static int seg_started[MAX_INPUT + 1];
static int ev_shot_pending;
static unsigned ev_shot_n;

void dev_init(void)
{
	explore_init();
	t0 = SDL_GetTicks();
	seg_started[0] = 1;
	shot_dir = getenv("NANPA2_SHOT_DIR");
	const char *s = getenv("NANPA2_SHOT_SCHEDULE");
	if (shot_dir && s)
		sscanf(s, "%u,%u,%u", &shot_start, &shot_interval, &shot_count);
	const char *in = getenv("NANPA2_INPUT");
	while (in && *in && ninput < MAX_INPUT) {
		struct ev *e = &input[ninput];
		int n = 0;
		if (sscanf(in, "%7[a-z],%u%n", e->kind, &e->ms, &n) >= 2) {
			in += n;
			if (!strcmp(e->kind, "sync")) {
				if (*in == ',')
					sscanf(in, ",%31[^;]%n", e->file, &n), in += n;
				e->seg = ++nseg;
			} else {
				e->seg = nseg;
				if (*in == ',')
					sscanf(in, ",%d,%d%n", &e->x, &e->y, &n), in += n;
			}
			ninput++;
			/* a click or key press is held for HOLD_MS, as by a hand: the
			 * game samples the button bits about once a frame */
			const char *rel = !strcmp(e->kind, "click") ? "up" : !strcmp(e->kind, "rclick") ? "rup"
					: !strcmp(e->kind, "key") ? "keyup" : NULL;
			if (rel && ninput < MAX_INPUT) {
				input[ninput] = *e;
				snprintf(input[ninput].kind, sizeof input[ninput].kind, "%s", rel);
				input[ninput].ms += HOLD_MS;
				ninput++;
			}
		}
		const char *semi = strchr(in, ';');
		if (!semi)
			break;
		in = semi + 1;
	}
}

int dev_scripted(void)
{
	return ninput > 0 || explore_on() || replay_replaying();
}

uint32_t dev_now(void)
{
	return SDL_GetTicks() - t0;
}

/* Clock for scripted input and screenshots: wall-clock ms, or with
 * NANPA2_INPUT_CLOCK=frames the game's own 16 ms tick counter (0x446292)
 * times 16, so a schedule lines up with the game however fast the host
 * runs or stalls. */
static uint32_t clock_now(void)
{
	static int frames = -1;
	if (frames < 0) {
		const char *c = getenv("NANPA2_INPUT_CLOCK");
		frames = (c && !strcmp(c, "frames")) || replay_det();
	}
	return frames ? RT(frame) * 16u : dev_now();
}

static void save_frame(const uint8_t *frame, const uint32_t pal[256], const char *path)
{
	SDL_Surface *s = SDL_CreateSurface(SCREEN_W, SCREEN_H, SDL_PIXELFORMAT_XRGB8888);
	for (int y = 0; y < SCREEN_H; y++) {
		uint32_t *row = (uint32_t *)((uint8_t *)s->pixels + y * s->pitch);
		for (int x = 0; x < SCREEN_W; x++)
			row[x] = pal[frame[y * SCREEN_W + x]];
	}
	SDL_SaveBMP(s, path);
	SDL_DestroySurface(s);
}

static void dev_frame_impl(const uint8_t *frame, const uint32_t pal[256], const uint8_t *prev, const uint32_t *prevpal, int have_prev)
{
	char path[1024];
	const uint8_t *prev_frame = prev;
	const uint32_t *prev_pal = prevpal;
	if (!shot_dir)
		return;
	if (ev_shot_pending) {
		ev_shot_pending = 0;
		snprintf(path, sizeof path, "%s/ev_%03u.bmp", shot_dir, ev_shot_n++);
		save_frame(frame, pal, path);
	}
	if (shot_done >= shot_count)
		return;
	uint32_t due = shot_start + shot_done * shot_interval;
	if (clock_now() < due)
		return;
	snprintf(path, sizeof path, "%s/shot_%03u.bmp", shot_dir, shot_done);
	save_frame(frame, pal, path);
	if (getenv("NANPA2_SHOT_PRESENT")) {
		int cw = 0, ch = 0;
		if (getenv("NANPA2_SHOT_SIZE"))
			sscanf(getenv("NANPA2_SHOT_SIZE"), "%dx%d", &cw, &ch);
		if (have_prev && !strcmp(getenv("NANPA2_SHOT_PRESENT"), "2")) {
			snprintf(path, sizeof path, "%s/prev_%03u.bmp", shot_dir, shot_done);
			save_frame(prev_frame, prev_pal, path);
		}
		snprintf(path, sizeof path, "%s/present_%03u.bmp", shot_dir, shot_done);
		present_capture(path, cw, ch);
		if (!strcmp(getenv("NANPA2_SHOT_PRESENT"), "2")) {
			/* the same frame once more with the game's own glyphs, for comparing */
			int saved = tl_get_mode();
			tl_set_mode(TEXT_BITMAP);
			snprintf(path, sizeof path, "%s/bitmap_%03u.bmp", shot_dir, shot_done);
			present_capture(path, cw, ch);
			tl_set_mode(saved);
		}
	}
	shot_done++;
}

void dev_frame(const uint8_t *frame, const uint32_t pal[256])
{
	/* the frame the presenter holds while this one is on its way is what present_capture shows */
	static uint8_t prev_frame[SCREEN_W * SCREEN_H];
	static uint32_t prev_pal[256];
	static int have_prev;
	dev_frame_impl(frame, pal, prev_frame, prev_pal, have_prev);
	if (shot_dir && getenv("NANPA2_SHOT_PRESENT") && !strcmp(getenv("NANPA2_SHOT_PRESENT"), "2")) {
		memcpy(prev_frame, frame, sizeof prev_frame);
		memcpy(prev_pal, pal, sizeof prev_pal);
		have_prev = 1;
	}
}

static int log_on(void)
{
	static int on = -1;
	if (on < 0)
		on = getenv("NANPA2_LOG") != NULL;
	return on;
}

static int ci_contains(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++)
		if (!SDL_strncasecmp(hay, needle, n))
			return 1;
	return 0;
}

/* The first pending sync fires when its trigger happens after its own
 * segment started; events of earlier segments that have not fired yet are
 * dropped (e.g. dialogue clicks once the next menu is up). */
/* Lowest index of an event that has not fired; scans start here, since a
 * long script is mostly fired events and both scans run very often (every
 * script opcode, every pump). */
static int first_live(void)
{
	static int lo;
	while (lo < ninput && input[lo].done)
		lo++;
	return lo;
}

static void sync_check(const char *name, int vm, uint32_t off)
{
	for (int i = first_live(); i < ninput; i++) {
		struct ev *e = &input[i];
		if (e->done || strcmp(e->kind, "sync"))
			continue;
		int is_vm = e->file[0] == '@';
		if (!seg_started[e->seg - 1] || is_vm != vm)
			return;
		if (vm ? strtoul(e->file + 1, NULL, 16) != off : !ci_contains(name, e->file))
			return;
		e->done = 1;
		seg_started[e->seg] = 1;
		seg_start[e->seg] = clock_now();
		for (int k = 0; k < i; k++)
			input[k].done = 1;
		if (log_on())
			fprintf(stderr, "dev: sync %s at %u ms frame %u (script RS %u RV %u RN %u)\n",
				e->file, seg_start[e->seg], RT(frame),
				G.save.reg['S' - 0x40], G.save.reg['V' - 0x40], G.save.reg['N' - 0x40]);
		return;
	}
}

void dev_file_opened(const char *name)
{
	{ extern void ctl_tlog(const char *, uint32_t); ctl_tlog(name, 0); }
	explore_file(name);
	sync_check(name, 0, 0);
}

void dev_vm_fetch(uint32_t off)
{
	replay_note_vm(off);
	explore_vm(off);
	if (nseg)
		sync_check(NULL, 1, off);
}

/* The script hit test (ext op 0x23, 0x4071a4) scans 10-byte entries
 * {code, x0, y0, x1, y1} up to code 0xffff and returns the code of the
 * rectangle under the mouse in register S. The table is built at run time,
 * so NANPA2_HOTSPOTS=1 logs it whenever it changes. */
void dev_hit_table(uint32_t table)
{
	static int on = -1;
	static uint8_t last[1000];
	static uint32_t last_at, last_len = ~0u;
	explore_hits(table);
	if (on < 0)
		on = getenv("NANPA2_HOTSPOTS") != NULL;
	if (!on)
		return;
	uint32_t n = 0;
	while (n < 100 && rd16(table + 10 * n) != 0xffff)
		n++;
	uint32_t len = 10 * n;
	if (table == last_at && len == last_len && !memcmp(last, g_ptr(table, len), len))
		return;
	last_at = table;
	last_len = len;
	memcpy(last, g_ptr(table, len), len);
	fprintf(stderr, "hotspots at %06x frame %u:", table, RT(frame));
	for (uint32_t i = 0; i < n; i++) {
		uint32_t e = table + 10 * i;
		fprintf(stderr, " %04x(%u,%u)-(%u,%u)", rd16(e), rd16(e + 2), rd16(e + 4),
			rd16(e + 6), rd16(e + 8));
	}
	fprintf(stderr, "\n");
}

/* NANPA2_CLICKTEST=1: set both click handlers (ext op 20 sets them; no
 * shipped script uses it) to a block `01 10 00` in the free end of the
 * script image, so that clicks run the VM's click handler path
 * (0x40613f-0x40616c, src/game/loop.c) in a scripted run. */
static void click_test(void)
{
	static int on = -1;
	if (on < 0)
		on = getenv("NANPA2_CLICKTEST") != NULL;
	if (!on)
		return;
	enum { BLOCK = 0xf000 }; /* above the largest MES file, within a u16 offset */
	wr8(0x435e78u + BLOCK, 0x01);
	wr8(0x435e78u + BLOCK + 1, 0x10);
	wr8(0x435e78u + BLOCK + 2, 0x00);
	RT_SET(click_handler[0], BLOCK);
	RT_SET(click_handler[1], BLOCK);
}

void dev_inject(void)
{
	explore_tick();
	click_test();
	uint32_t now = clock_now();
	for (int i = first_live(); i < ninput; i++) {
		struct ev *e = &input[i];
		if (e->done)
			continue;
		/* segments start in order: nothing after an unfired sync is live */
		if (!strcmp(e->kind, "sync"))
			break;
		if (!seg_started[e->seg] || now - seg_start[e->seg] < e->ms)
			continue;
		e->done = 1;
		if (log_on())
			fprintf(stderr, "dev: %s %d,%d at %u ms frame %u\n", e->kind, e->x, e->y, now,
				RT(frame));
		uint32_t lp = ((uint32_t)e->y << 16) | (uint32_t)e->x;
		if (!strcmp(e->kind, "move")) {
			win_post_ext(0x200, 0, lp);
		} else if (!strcmp(e->kind, "click") || !strcmp(e->kind, "down")) {
			win_post_ext(0x200, 0, lp);
			win_post_ext(0x201, 1, lp);
		} else if (!strcmp(e->kind, "up")) {
			win_post_ext(0x202, 0, lp);
		} else if (!strcmp(e->kind, "rclick")) {
			if (getenv("NANPA2_LATLOG"))
				fprintf(stderr, "RP frame %u seg%d.%d scripted\n", RT(frame), e->seg, e->y);
			win_post_ext(0x200, 0, lp);
			win_post_ext(0x204, 2, lp);
		} else if (!strcmp(e->kind, "rup")) {
			win_post_ext(0x205, 0, lp);
		} else if (!strcmp(e->kind, "key")) {
			win_post_ext(0x100, (uint32_t)e->x, 0);
		} else if (!strcmp(e->kind, "keyup")) {
			win_post_ext(0x101, (uint32_t)e->x, 0);
		} else if (!strcmp(e->kind, "shot")) {
			ev_shot_pending = 1;
		} else if (!strcmp(e->kind, "quit")) {
			win_post_ext(0x12, 0, 0);
		}
	}
}

/* Save/load round trip through the game's own code: the FLAG serializer (game/state_view.h) and the
 * file functions the script save and load opcodes (04 21) use. A known pattern in the save image is
 * exported, written to the slot FLAG9 (which the game never names: its slots are FLAG0-6), read back
 * and imported over a scrambled image, which must export the same bytes again; and the plain
 * loader (0x401050) must read the file as it is. */
int dev_save_selftest(void)
{
	char *name = (char *)G.rt.string_buf;
	memcpy(name, "FLAG9", 6);
	uint8_t *raw = (uint8_t *)&G.save;
	for (uint32_t i = 0; i < STATE_SAVE_SIZE; i++)
		raw[i] = (uint8_t)((i * 7 + 3) & 0xff);
	uint8_t src[STATE_SAVE_SIZE], back[STATE_SAVE_SIZE], again[STATE_SAVE_SIZE];
	state_save_export(src);
	game_file_save(name, src, STATE_SAVE_SIZE);
	int ok = game_file_read(name, back, sizeof back) == STATE_SAVE_SIZE && !memcmp(src, back, sizeof src);
	memset(&G.save, 0xa5, sizeof G.save);
	state_save_import(back, sizeof back);
	state_save_export(again);
	ok = ok && !memcmp(src, again, sizeof src);
	uint8_t *dst = G.img + 0x1000;
	game_file_load(name, dst, STATE_SAVE_SIZE);
	ok = ok && !memcmp(src, dst, sizeof src);
	fprintf(stderr, "save selftest: %s\n", ok ? "ok" : "MISMATCH");
	return ok ? 0 : 1;
}

/* ---- hooks: how platform code and trap.c reach this layer (hooks.h) ---- */

static int hook_external_msg(uint32_t msg, uint32_t wparam, uint32_t lparam)
{
	return replay_external(msg, wparam, lparam, 0);
}

static void hook_file(const char *name)
{
	dev_file_opened(name);
	replay_note_file(name);
}

void dev_install_hooks(void)
{
	dev_hooks = (struct dev_hooks){
		.det = replay_det,
		.replaying = replay_replaying,
		.virtual_ms = replay_ms,
		.pump = replay_pump,
		.external = hook_external_msg,
		.file_opened = hook_file,
		.frame = dev_frame,
		.inject = dev_inject,
		.scripted = dev_scripted,
		.now = dev_now,
		.beat = status_main,
		.logo = status_set_logo,
		.vm_fetch = dev_vm_fetch,
		.hit_table = dev_hit_table,
	};
}
