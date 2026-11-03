/* Developer hooks for scripted, headless testing (environment variables):
 *   NANPA2_SHOT_DIR=dir NANPA2_SHOT_SCHEDULE=start_ms,interval_ms,count
 *       save the screen as dir/shot_NNN.bmp on that schedule
 *   NANPA2_INPUT="click,MS,X,Y;rclick,MS,X,Y;move,MS,X,Y;key,MS,VK;quit,MS"
 *       timed input in 640x480 screen coordinates, MS after start; clicks and
 *       keys are held for HOLD_MS
 *       sync,0,NAME   later events count from when the game opens file NAME
 *                     (e.g. DATA\NAME.GP4; case-insensitive substring)
 *       shot,MS       save the next frame as dir/ev_NNN.bmp */
#include "dev.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "platform/present.h"

static const char *shot_dir;
static uint32_t shot_start, shot_interval, shot_count, shot_done;
static uint32_t t0;

#define MAX_INPUT 512
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

uint32_t dev_now(void)
{
	return SDL_GetTicks() - t0;
}

static void save_frame(const uint8_t *frame, const uint32_t pal[256], const char *path)
{
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_W, SCREEN_H, 32, SDL_PIXELFORMAT_XRGB8888);
	for (int y = 0; y < SCREEN_H; y++) {
		uint32_t *row = (uint32_t *)((uint8_t *)s->pixels + y * s->pitch);
		for (int x = 0; x < SCREEN_W; x++)
			row[x] = pal[frame[y * SCREEN_W + x]];
	}
	SDL_SaveBMP(s, path);
	SDL_FreeSurface(s);
}

void dev_frame(const uint8_t *frame, const uint32_t pal[256])
{
	char path[1024];
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
	if (dev_now() < due)
		return;
	snprintf(path, sizeof path, "%s/shot_%03u.bmp", shot_dir, shot_done);
	save_frame(frame, pal, path);
	shot_done++;
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

void dev_file_opened(const char *name)
{
	for (int i = 0; i < ninput; i++) {
		struct ev *e = &input[i];
		if (e->done || strcmp(e->kind, "sync"))
			continue;
		/* only the first pending sync, and only once its own segment runs */
		if (seg_started[e->seg - 1] && ci_contains(name, e->file)) {
			e->done = 1;
			seg_started[e->seg] = 1;
			seg_start[e->seg] = dev_now();
			if (log_on())
				fprintf(stderr, "dev: sync %s at %u ms\n", e->file, seg_start[e->seg]);
		}
		return;
	}
}

void dev_inject(void)
{
	uint32_t now = dev_now();
	for (int i = 0; i < ninput; i++) {
		struct ev *e = &input[i];
		if (e->done || !seg_started[e->seg] || !strcmp(e->kind, "sync") ||
		    now - seg_start[e->seg] < e->ms)
			continue;
		e->done = 1;
		if (log_on())
			fprintf(stderr, "dev: %s %d,%d at %u ms frame %u\n", e->kind, e->x, e->y, now,
				rd32(0x446292));
		uint32_t lp = ((uint32_t)e->y << 16) | (uint32_t)e->x;
		if (!strcmp(e->kind, "move")) {
			win32_post_message(0x200, 0, lp);
		} else if (!strcmp(e->kind, "click")) {
			win32_post_message(0x200, 0, lp);
			win32_post_message(0x201, 1, lp);
		} else if (!strcmp(e->kind, "up")) {
			win32_post_message(0x202, 0, lp);
		} else if (!strcmp(e->kind, "rclick")) {
			win32_post_message(0x200, 0, lp);
			win32_post_message(0x204, 2, lp);
		} else if (!strcmp(e->kind, "rup")) {
			win32_post_message(0x205, 0, lp);
		} else if (!strcmp(e->kind, "key")) {
			win32_post_message(0x100, (uint32_t)e->x, 0);
		} else if (!strcmp(e->kind, "keyup")) {
			win32_post_message(0x101, (uint32_t)e->x, 0);
		} else if (!strcmp(e->kind, "shot")) {
			ev_shot_pending = 1;
		} else if (!strcmp(e->kind, "quit")) {
			win32_post_message(0x12, 0, 0);
		}
	}
}
