/* The start save: see startsave.h. */
#include "dev/startsave.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev/dev.h"
#include "dev/explore.h"
#include "dev/replay.h"
#include "fsys.h"
#include "game/gmem.h"
#include "game/startup.h"
#include "game/state.h"
#include "game/state_view.h"
#include "hooks.h"
#include "platform/clock.h"
#include "platform/files.h"
#include "platform/offline.h"
#include "platform/present.h"
#include "platform/surface.h"
#include "platform/window.h"

static int exists(const char *path)
{
	SDL_PathInfo info;
	return SDL_GetPathInfo(path, &info);
}

static void path_of(char *out, size_t cap, const char *dir, const char *name)
{
	snprintf(out, cap, "%s/%s", dir, name);
}

/* ---- the gate ---- */

static int saves_in(const char *dir)
{
	for (int i = 0; i < 10; i++) {
		char p[1200], n[16];
		snprintf(n, sizeof n, "flag%d", i);
		path_of(p, sizeof p, dir, n);
		if (exists(p))
			return 1;
	}
	return 0;
}

/* The install's own FLAG files (data1.cab): absent, or every one that exists the same bytes (the
 * blank). Anything else is somebody's save. */
static int game_folder_blank(void)
{
	static unsigned char first[STATE_SAVE_SIZE + 1], cur[STATE_SAVE_SIZE + 1];
	size_t first_len = 0;
	int have = 0;
	for (int i = 0; i < 10; i++) {
		char n[16];
		snprintf(n, sizeof n, "FLAG%d", i);
		FILE *f = fsys_open(n);
		if (!f)
			continue;
		size_t len = fread(have ? cur : first, 1, STATE_SAVE_SIZE + 1, f);
		fclose(f);
		if (!have) {
			have = 1;
			first_len = len;
		} else if (len != first_len || memcmp(first, cur, len)) {
			return 0;
		}
	}
	return 1;
}

#define MARKER "startsave-off"

int startsave_saves_offered(const char *save_dir)
{
	char p[1200];
	path_of(p, sizeof p, save_dir, MARKER);
	return saves_in(save_dir) && !exists(p);
}

void startsave_decline(const char *save_dir)
{
	for (int i = 0; i < 7; i++) {
		char p[1200], n[16];
		snprintf(n, sizeof n, "flag%d", i);
		path_of(p, sizeof p, save_dir, n);
		if (exists(p) && SDL_RemovePath(p))
			fprintf(stderr, "start-save: removed %s\n", n);
	}
	char p[1200];
	path_of(p, sizeof p, save_dir, MARKER);
	FILE *f = fopen(p, "wb");
	if (f) {
		fputs("the player removed the saves and chose to start over: no start save is made while this file exists\n", f);
		fclose(f);
	}
	fprintf(stderr, "start-save: saves removed by the player, start save off (%s)\n", MARKER);
}

int startsave_wanted(const char *save_dir, int check_game_folder)
{
	char marker[1200];
	path_of(marker, sizeof marker, save_dir, MARKER);
	if (saves_in(save_dir) || (check_game_folder && exists(marker)))
		return 0;
	return !check_game_folder || game_folder_blank();
}

/* ---- the run ---- */

enum { SEED = 31, FRAME_CAP = 450000 }; /* 107 minutes of game time; the Prologue takes a fraction */

static struct {
	const char *stage;
	int open1, found, quit, failed, show;
	uint32_t start_ms, shown_ms, next_poll, cap, last_frame;
	char flag6[1200];
} ss;

static int ss_det(void) { return 1; }
static int ss_replaying(void) { return 0; }
static int ss_scripted(void) { return 1; }

static int ss_external(uint32_t msg, uint32_t wp, uint32_t lp)
{
	(void)wp;
	(void)lp;
	if (msg == WM_QUIT)
		ss.quit = 1; /* the host asks to close: only this gets in */
	return 1;
}

static int ci_equal(const char *a, const char *b)
{
	return !SDL_strcasecmp(a, b);
}

static void ss_file(const char *name)
{
	dev_file_opened(name);
	const char *b = name;
	for (const char *q = name; *q; q++)
		if (*q == '\\' || *q == '/' || *q == ':')
			b = q + 1;
	/* OPEN1.MES: the Prologue's end. Its save op writes FLAG6; the scene saves before it
	 * (the 存檔 items of the S scenes, FLAG0 and FLAG6) are not the result. */
	if (!ss.open1 && ci_equal(b, "OPEN1.MES")) {
		ss.open1 = 1;
		char p[1200];
		path_of(p, sizeof p, ss.stage, "flag0");
		SDL_RemovePath(p);
		SDL_RemovePath(ss.flag6);
		if (getenv("NANPA2_START_SAVE_LOG"))
			fprintf(stderr, "start-save: OPEN1.MES at frame %u\n", RT(frame));
	}
}

/* a plain "preparing" screen: a bar sweeping along a frame, no text (no glyphs offline) */
static void show_progress(uint32_t now)
{
	static uint8_t frame[SCREEN_W * SCREEN_H];
	static uint32_t pal[256];
	pal[0] = 0x101018;
	pal[1] = 0x606878;
	pal[2] = 0xe0e4f0;
	memset(frame, 0, sizeof frame);
	enum { X0 = 220, X1 = 420, Y0 = 232, Y1 = 248 };
	for (int y = Y0; y < Y1; y++)
		for (int x = X0; x < X1; x++)
			frame[y * SCREEN_W + x] = (y == Y0 || y == Y1 - 1 || x == X0 || x == X1 - 1) ? 1 : 0;
	int w = 40, span = X1 - X0 - 4 - w;
	int t = (int)((now / 12) % (uint32_t)(2 * span));
	int x0 = X0 + 2 + (t < span ? t : 2 * span - t);
	for (int y = Y0 + 2; y < Y1 - 2; y++)
		for (int x = x0; x < x0 + w; x++)
			frame[y * SCREEN_W + x] = 2;
	present_frame(frame, pal);
}

/* NANPA2_START_SAVE_SHOTS=dir,every: save the screen every `every` frames (looking at a run) */
static void ss_shot(uint32_t f)
{
	static const char *spec;
	static char dir[900];
	static uint32_t every, last = ~0u;
	if (!spec) {
		spec = getenv("NANPA2_START_SAVE_SHOTS");
		if (!spec)
			spec = "";
		sscanf(spec, "%899[^,],%u", dir, &every);
	}
	if (!every || f / every == last)
		return;
	last = f / every;
	static uint8_t frame[SCREEN_W * SCREEN_H];
	static uint32_t pal[256];
	if (!screen_snapshot(frame, pal))
		return;
	int nz = 0, pz = 0;
	for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
		nz += frame[i] != 0;
	for (int i = 0; i < 256; i++)
		pz += pal[i] != 0;
	fprintf(stderr, "start-save: frame %u nonzero pixels %d palette %d fade %u\n", f, nz, pz, (unsigned)SC(fade_active));
	SDL_Surface *s = SDL_CreateSurface(SCREEN_W, SCREEN_H, SDL_PIXELFORMAT_XRGB8888);
	for (int y = 0; y < SCREEN_H; y++)
		for (int x = 0; x < SCREEN_W; x++)
			((uint32_t *)((uint8_t *)s->pixels + y * s->pitch))[x] = pal[frame[y * SCREEN_W + x]];
	char p[1000];
	snprintf(p, sizeof p, "%s/ss_%06u.bmp", dir, f);
	SDL_SaveBMP(s, p);
	SDL_DestroySurface(s);
}

static void ss_inject(void)
{
	/* everything the bot does is stamped in game frames: once per frame is enough */
	if (RT(frame) == ss.last_frame)
		return;
	ss.last_frame = RT(frame);
	dev_inject();
	ss_shot(RT(frame));
	if (ss.found || ss.failed || ss.quit)
		return;
	uint32_t f = RT(frame);
	if (f >= ss.cap) {
		ss.failed = 1;
		win_post(WM_QUIT, 0, 0);
		return;
	}
	if (ss.open1 && f >= ss.next_poll) {
		ss.next_poll = f + 2;
		SDL_PathInfo info;
		if (SDL_GetPathInfo(ss.flag6, &info) && info.size == STATE_SAVE_SIZE) {
			ss.found = 1;
			win_post(WM_QUIT, 0, 0);
			return;
		}
	}
	if (ss.show) {
		uint32_t now = SDL_GetTicks();
		if (now - ss.start_ms > 2000 && now - ss.shown_ms >= 80) {
			ss.shown_ms = now;
			show_progress(now);
		}
	}
}

static int read_file(const char *path, uint8_t *buf, size_t n)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	size_t got = fread(buf, 1, n + 1, f);
	fclose(f);
	return got == n;
}

static int write_file(const char *path, const uint8_t *buf, size_t n)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return 0;
	int ok = fwrite(buf, 1, n, f) == n;
	return fclose(f) == 0 && ok;
}

static void clear_stage(const char *stage)
{
	for (int i = 0; i < 10; i++) {
		char p[1200], n[16];
		snprintf(n, sizeof n, "flag%d", i);
		path_of(p, sizeof p, stage, n);
		SDL_RemovePath(p);
	}
}

int startsave_make(const char *save_dir, int show)
{
	char stage[1200];
	path_of(stage, sizeof stage, save_dir, ".start-save");
	memset(&ss, 0, sizeof ss);
	ss.stage = stage;
	path_of(ss.flag6, sizeof ss.flag6, stage, "flag6");
	ss.show = show;
	ss.last_frame = ~0u;
	ss.cap = FRAME_CAP;
	uint32_t seed = SEED;
	if (getenv("NANPA2_START_SAVE_SEED"))
		seed = (uint32_t)strtoul(getenv("NANPA2_START_SAVE_SEED"), NULL, 0);
	if (getenv("NANPA2_START_SAVE_FRAMES"))
		ss.cap = (uint32_t)strtoul(getenv("NANPA2_START_SAVE_FRAMES"), NULL, 0);
	SDL_CreateDirectory(save_dir);
	clear_stage(stage);
	if (!SDL_CreateDirectory(stage)) {
		fprintf(stderr, "start-save: cannot make %s: %s\n", stage, SDL_GetError());
		return 0;
	}

	/* the dev layer for the duration: deterministic clock, the policy bot as the only input */
	struct dev_hooks keep = dev_hooks;
	dev_hooks = (struct dev_hooks){
		.det = ss_det,
		.replaying = ss_replaying,
		.virtual_ms = replay_ms,
		.external = ss_external,
		.file_opened = ss_file,
		.inject = ss_inject,
		.scripted = ss_scripted,
		.now = dev_now,
		.vm_fetch = dev_vm_fetch,
		.hit_table = dev_hit_table,
	};
	state_init();
	files_offline_begin(stage);
	explore_policy_begin(seed);
	offline_begin();
	offline_set_fast_forward(getenv("NANPA2_START_SAVE_NOFF") == NULL);
	ss.start_ms = SDL_GetTicks();
	game_main();
	uint32_t ms = SDL_GetTicks() - ss.start_ms;
	uint32_t frames = RT(frame);
	offline_end();
	explore_policy_end();
	files_offline_end();
	dev_hooks = keep;
	state_init(); /* the real session starts from the defaults */

	int ret = 0;
	if (ss.quit) {
		ret = -1;
	} else if (ss.found) {
		static uint8_t img[STATE_SAVE_SIZE];
		char p0[1200], f0[1200], f6[1200];
		path_of(p0, sizeof p0, stage, "flag0");
		path_of(f0, sizeof f0, save_dir, "flag0");
		path_of(f6, sizeof f6, save_dir, "flag6");
		/* FLAG6 is what the game wrote; FLAG0 the same image, so that Data Load lights diary 1 */
		if (read_file(ss.flag6, img, sizeof img) && write_file(p0, img, sizeof img) && !exists(f0) &&
		    !exists(f6) && SDL_RenamePath(ss.flag6, f6)) {
			if (SDL_RenamePath(p0, f0)) {
				ret = 1;
			} else {
				SDL_RemovePath(f6);
			}
		}
	}
	clear_stage(stage);
	SDL_RemovePath(stage);
	fprintf(stderr, "start-save: %s after %u frames, %.1f s\n",
		ret > 0 ? "written" : ret < 0 ? "stopped by quit" : ss.failed ? "FAILED (frame cap)" : "FAILED",
		frames, ms / 1000.0);
	return ret;
}
