/* Developer hooks for scripted, headless testing (environment variables):
 *   NANPA2_SHOT_DIR=dir NANPA2_SHOT_SCHEDULE=start_ms,interval_ms,count
 *       save the screen as dir/shot_NNN.bmp on that schedule
 *   NANPA2_SHOT_PRESENT=1
 *       also save what the presenter draws (CRT included) at the window's
 *       drawable size as dir/present_NNN.bmp
 *   NANPA2_INPUT="click,MS,X,Y;rclick,MS,X,Y;move,MS,X,Y;key,MS,VK;quit,MS"
 *       timed input in 640x480 screen coordinates, MS after start; clicks and
 *       keys are held for HOLD_MS */
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
} input[MAX_INPUT];
static int ninput;

void dev_init(void)
{
	t0 = SDL_GetTicks();
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
			if (*in == ',')
				sscanf(in, ",%d,%d%n", &e->x, &e->y, &n), in += n;
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

void dev_frame(const uint8_t *frame, const uint32_t pal[256])
{
	if (!shot_dir || shot_done >= shot_count)
		return;
	uint32_t due = shot_start + shot_done * shot_interval;
	if (dev_now() < due)
		return;
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_W, SCREEN_H, 32, SDL_PIXELFORMAT_XRGB8888);
	for (int y = 0; y < SCREEN_H; y++) {
		uint32_t *row = (uint32_t *)((uint8_t *)s->pixels + y * s->pitch);
		for (int x = 0; x < SCREEN_W; x++)
			row[x] = pal[frame[y * SCREEN_W + x]];
	}
	char path[1024];
	snprintf(path, sizeof path, "%s/shot_%03u.bmp", shot_dir, shot_done);
	SDL_SaveBMP(s, path);
	SDL_FreeSurface(s);
	if (getenv("NANPA2_SHOT_PRESENT")) {
		snprintf(path, sizeof path, "%s/present_%03u.bmp", shot_dir, shot_done);
		present_capture(path, 0, 0);
	}
	shot_done++;
}

void dev_inject(void)
{
	uint32_t now = dev_now();
	for (int i = 0; i < ninput; i++) {
		struct ev *e = &input[i];
		if (e->done || now < e->ms)
			continue;
		e->done = 1;
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
		} else if (!strcmp(e->kind, "quit")) {
			win32_post_message(0x12, 0, 0);
		}
	}
}
