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

/* Window (point) coordinates to screen pixels, using the window size. */
static inline int present_map_point(SDL_Window *win, int wx, int wy, int *sx, int *sy)
{
	int ww, wh, x, y, w, h;
	SDL_GetWindowSize(win, &ww, &wh);
	present_fit(ww, wh, &x, &y, &w, &h);
	if (w <= 0 || h <= 0)
		return 0;
	*sx = (wx - x) * SCREEN_W / w;
	*sy = (wy - y) * SCREEN_H / h;
	int inside = *sx >= 0 && *sy >= 0 && *sx < SCREEN_W && *sy < SCREEN_H;
	if (*sx < 0) *sx = 0;
	if (*sy < 0) *sy = 0;
	if (*sx >= SCREEN_W) *sx = SCREEN_W - 1;
	if (*sy >= SCREEN_H) *sy = SCREEN_H - 1;
	return inside;
}

#endif
