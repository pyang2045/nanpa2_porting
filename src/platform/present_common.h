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

/* The CRT simulation is a flat-face (Sony FD Trinitron style) tube: no
 * curvature, so window points map linearly onto the screen with or without
 * it. */

/* Window (point) coordinates to screen pixels, using the window size.
 * Returns 0 (and clamps) outside the picture. */
static inline int present_map_point(SDL_Window *win, int wx, int wy, int *sx, int *sy)
{
	int ww, wh, x, y, w, h;
	SDL_GetWindowSize(win, &ww, &wh);
	present_fit(ww, wh, &x, &y, &w, &h);
	if (w <= 0 || h <= 0)
		return 0;
	*sx = (int)SDL_floorf((wx - x + 0.5f) * SCREEN_W / (float)w);
	*sy = (int)SDL_floorf((wy - y + 0.5f) * SCREEN_H / (float)h);
	int inside = *sx >= 0 && *sy >= 0 && *sx < SCREEN_W && *sy < SCREEN_H;
	if (*sx < 0) *sx = 0;
	if (*sy < 0) *sy = 0;
	if (*sx >= SCREEN_W) *sx = SCREEN_W - 1;
	if (*sy >= SCREEN_H) *sy = SCREEN_H - 1;
	return inside;
}

/* Inverse of present_map_point: the window point at the centre of screen
 * pixel (sx, sy). */
static inline void present_unmap_point(SDL_Window *win, int sx, int sy, int *wx, int *wy)
{
	int ww, wh, x, y, w, h;
	SDL_GetWindowSize(win, &ww, &wh);
	present_fit(ww, wh, &x, &y, &w, &h);
	*wx = x + (int)SDL_floorf((sx + 0.5f) * w / SCREEN_W);
	*wy = y + (int)SDL_floorf((sy + 0.5f) * h / SCREEN_H);
}

static inline void present_common_set_fullscreen(SDL_Window *win, int on)
{
	SDL_SetWindowFullscreen(win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static inline int present_common_get_fullscreen(SDL_Window *win)
{
	return (SDL_GetWindowFlags(win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
}

/* Shared hotkey handling (see present_hotkey). The key that completes a
 * chord is remembered so that its release is swallowed too, even if the
 * modifier was let go first. */
static inline int present_common_hotkey(SDL_Window *win, const SDL_KeyboardEvent *k,
					void (*toggle_crt)(void))
{
	static SDL_Keycode held;
	SDL_Keycode sym = k->keysym.sym;
	Uint16 mod = k->keysym.mod;
	if (k->type == SDL_KEYUP) {
		if (sym == SDLK_F12)
			return 1;
		if (held && sym == held) {
			held = 0;
			return 1;
		}
		return 0;
	}
	if (sym == SDLK_F12) {
		if (!k->repeat)
			toggle_crt();
		return 1;
	}
	int alt_enter = (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) && (mod & KMOD_ALT);
#ifdef __APPLE__
	int cmd_f = sym == SDLK_f && (mod & KMOD_GUI);
#else
	int cmd_f = 0;
#endif
	if (!alt_enter && !cmd_f)
		return 0;
	held = sym;
	if (!k->repeat)
		present_common_set_fullscreen(win, !present_common_get_fullscreen(win));
	return 1;
}

#endif
