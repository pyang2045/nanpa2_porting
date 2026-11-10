/* Platform API: surfaces, the palette and the screen. See surface.h. */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hooks.h"
#include "platform/surface.h"
#include "platform/clock.h"
#include "platform/window.h"
#include "platform/present.h"
#include "trap.h"

#define MAX_SURF 16

static struct surf {
	uint32_t handle;
	uint8_t *mem, *pixels; /* mem: the allocation with the slack, pixels: the surface within it */
	int w, h, pitch;
	int primary;
	uint32_t ckey_lo, ckey_hi;
	int has_ckey;
} surfs[MAX_SURF];
static int nsurf;
static uint8_t palette[256][4]; /* red, green, blue, flags */

uint32_t surf_create(int w, int h, int primary)
{
	if (nsurf >= MAX_SURF || w <= 0 || h <= 0)
		return 0;
	struct surf *s = &surfs[nsurf];
	s->w = w;
	s->h = h;
	s->pitch = (w + 3) & ~3;
	s->primary = primary;
	s->mem = calloc(1, (size_t)s->pitch * (size_t)h + 2u * SURF_SLACK);
	if (!s->mem)
		return 0;
	s->pixels = s->mem + SURF_SLACK;
	s->handle = primary ? SURF_PRIMARY_HANDLE : 0x2000u + 0x10u * (uint32_t)nsurf;
	s->has_ckey = 0;
	nsurf++;
	return s->handle;
}

static struct surf *surf_of(uint32_t handle)
{
	for (int i = 0; i < nsurf; i++)
		if (surfs[i].handle == handle)
			return &surfs[i];
	return NULL;
}

static struct surf *surf_checked(uint32_t handle)
{
	struct surf *s = surf_of(handle);
	if (!s)
		game_trap(0, "a surface number that names no surface");
	return s;
}

uint8_t *surf_lock(uint32_t surface, int *pitch)
{
	struct surf *s = surf_checked(surface);
	if (pitch)
		*pitch = s->pitch;
	return s->pixels;
}

int surf_size(uint32_t surface, int *w, int *h)
{
	struct surf *s = surf_of(surface);
	if (!s)
		return 0;
	*w = s->w;
	*h = s->h;
	return 1;
}

void surf_set_colorkey(uint32_t surface, int enabled, uint32_t lo, uint32_t hi)
{
	struct surf *s = surf_checked(surface);
	s->has_ckey = enabled;
	s->ckey_lo = lo;
	s->ckey_hi = hi;
}

int surf_has_colorkey(uint32_t surface)
{
	return surf_checked(surface)->has_ckey;
}

void surf_blt_fast(uint32_t dst, int x, int y, uint32_t src, int l, int t, int r, int b, int keyed)
{
	struct surf *d = surf_checked(dst), *s = surf_checked(src);
	int key = keyed && s->has_ckey;
	for (int yy = t; yy < b; yy++) {
		int dy = y + (yy - t);
		if (yy < 0 || yy >= s->h || dy < 0 || dy >= d->h)
			continue;
		for (int xx = l; xx < r; xx++) {
			int dx = x + (xx - l);
			if (xx < 0 || xx >= s->w || dx < 0 || dx >= d->w)
				continue;
			uint32_t v = s->pixels[yy * s->pitch + xx];
			if (key && v >= s->ckey_lo && v <= s->ckey_hi)
				continue;
			d->pixels[dy * d->pitch + dx] = (uint8_t)v;
		}
	}
}

void surf_fill(uint32_t surface, int l, int t, int r, int b, uint32_t colour)
{
	struct surf *s = surf_checked(surface);
	for (int y = t < 0 ? 0 : t; y < b && y < s->h; y++)
		for (int x = l < 0 ? 0 : l; x < r && x < s->w; x++)
			s->pixels[y * s->pitch + x] = (uint8_t)colour;
}

void pal_set(uint32_t start, uint32_t count, const uint8_t *entries)
{
	for (uint32_t i = 0; i < count && start + i < 256; i++)
		memcpy(palette[start + i], entries + 4 * i, 4);
}

int screen_snapshot(uint8_t *frame, uint32_t pal[256])
{
	struct surf *p = NULL;
	for (int i = 0; i < nsurf; i++)
		if (surfs[i].primary)
			p = &surfs[i];
	if (!p)
		return 0;
	for (int y = 0; y < SCREEN_H && y < p->h; y++)
		memcpy(frame + y * SCREEN_W, p->pixels + y * p->pitch, SCREEN_W);
	for (int i = 0; i < 256; i++)
		pal[i] = ((uint32_t)palette[i][0] << 16) | ((uint32_t)palette[i][1] << 8) | palette[i][2];
	return 1;
}

void screen_present(int force)
{
	static uint32_t last;
	static uint8_t frame[SCREEN_W * SCREEN_H];
	static uint32_t pal[256];
	uint32_t now = SDL_GetTicks();
	if (!force && now - last < 16)
		return;
	if (!screen_snapshot(frame, pal))
		return;
	last = now;
	hook_frame(frame, pal);
	/* NANPA2_HEADLESS: the presenter keeps the frame in memory (no window,
	 * no GPU, see present_common.h) */
	present_frame(frame, pal);
	clock_lat("present");
}

void screen_wait_vblank(void)
{
	win_run_timer();
	screen_present(0);
}
