#ifndef NANPA2_PRESENT_COMMON_H
#define NANPA2_PRESENT_COMMON_H

#include "present.h"

/* Largest SCREEN_W:SCREEN_H rectangle centred in a dw x dh drawable. */
static inline void present_fit(int dw, int dh, int *x, int *y, int *w, int *h)
{
	if (dw * SCREEN_H > dh * SCREEN_W) {
		*h = dh;
		*w = dh * SCREEN_W / SCREEN_H;
	} else {
		*w = dw;
		*h = dw * SCREEN_H / SCREEN_W;
	}
	*x = (dw - *w) / 2;
	*y = (dh - *h) / 2;
}

/* CRT barrel curvature: maps a point of the displayed picture (0..1) to the
 * point of the screen shown there. The Metal shader uses the same formula. */
#define CRT_WARP_X (1.0f / 32.0f)
#define CRT_WARP_Y (1.0f / 24.0f)

static inline void present_crt_warp(float *u, float *v)
{
	float x = *u * 2.0f - 1.0f, y = *v * 2.0f - 1.0f;
	float wx = x * (1.0f + y * y * CRT_WARP_X), wy = y * (1.0f + x * x * CRT_WARP_Y);
	*u = wx * 0.5f + 0.5f;
	*v = wy * 0.5f + 0.5f;
}

/* Window (point) coordinates to screen pixels, using the window size.
 * warp: apply the CRT curvature. */
static inline int present_map_point(SDL_Window *win, int wx, int wy, int warp, int *sx, int *sy)
{
	int ww, wh, x, y, w, h;
	SDL_GetWindowSize(win, &ww, &wh);
	present_fit(ww, wh, &x, &y, &w, &h);
	if (w <= 0 || h <= 0)
		return 0;
	float u = (wx - x + 0.5f) / (float)w, v = (wy - y + 0.5f) / (float)h;
	if (warp)
		present_crt_warp(&u, &v);
	*sx = (int)SDL_floorf(u * SCREEN_W);
	*sy = (int)SDL_floorf(v * SCREEN_H);
	int inside = *sx >= 0 && *sy >= 0 && *sx < SCREEN_W && *sy < SCREEN_H;
	if (*sx < 0) *sx = 0;
	if (*sy < 0) *sy = 0;
	if (*sx >= SCREEN_W) *sx = SCREEN_W - 1;
	if (*sy >= SCREEN_H) *sy = SCREEN_H - 1;
	return inside;
}

#endif
