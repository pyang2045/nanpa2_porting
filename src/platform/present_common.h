#ifndef NANPA2_PRESENT_COMMON_H
#define NANPA2_PRESENT_COMMON_H

#include "present.h"
#include "platform/textlayer.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ---- status counters (present_stats) ---- */

static struct present_stats pstats = { .occluded = -1, .os_windows = -1, .luma_shown = -1, .app_hidden = -1,
				       .luma_probe = -1, .luma_probe_src = -1 };

/* Mean luminance of a frame, sampled sparsely (about 8000 pixels). */
static inline int present_luma_indexed(const uint8_t *pixels, const uint32_t pal[256])
{
	uint32_t sum = 0, n = 0;
	for (int i = 0; i < SCREEN_W * SCREEN_H; i += 37, n++) {
		uint32_t c = pal[pixels[i]];
		sum += (((c >> 16) & 0xff) * 54 + ((c >> 8) & 0xff) * 183 + (c & 0xff) * 19) >> 8;
	}
	return (int)(sum / n);
}

static inline int present_luma_rgb(const uint32_t *xrgb)
{
	uint32_t sum = 0, n = 0;
	for (int i = 0; i < SCREEN_W * SCREEN_H; i += 37, n++) {
		uint32_t c = xrgb[i];
		sum += (((c >> 16) & 0xff) * 54 + ((c >> 8) & 0xff) * 183 + (c & 0xff) * 19) >> 8;
	}
	return (int)(sum / n);
}

/* A frame was given to the presenter. */
static inline void present_note_in(int luma)
{
	pstats.frames_in++;
	pstats.luma_in = luma;
}

/* The last frame given reached the window. */
static inline void present_note_shown(void)
{
	uint32_t now = SDL_GetTicks();
	pstats.presented++;
	pstats.last_present = now ? now : 1;
	pstats.luma_shown = pstats.luma_in;
	if (pstats.luma_in >= PRESENT_BLACK_LUMA)
		pstats.last_nonblack = pstats.last_present;
}

/* The offscreen probe (see struct present_stats) rendered the last frame
 * given, with mean output luminance luma. */
static inline void present_note_probe(int luma)
{
	uint32_t now = SDL_GetTicks();
	pstats.probes++;
	pstats.last_probe = now ? now : 1;
	pstats.luma_probe = luma;
	pstats.luma_probe_src = pstats.luma_in;
	if (luma >= PRESENT_BLACK_LUMA)
		pstats.last_probe_nonblack = pstats.last_probe;
}

/* ---- headless presenter (NANPA2_HEADLESS) ----
 *
 * Headless runs use SDL's dummy video driver and no GPU at all: with the
 * screen locked, the window server and GPU setup (SDL_Init's display link,
 * CAMetalLayer setDevice) can block the process uninterruptibly. Frames
 * are kept in memory; about once a second the probe converts the last one
 * to RGB in C (palette lookup) and notes its luminance, so the status file
 * shows that the game draws; present_capture saves it (nearest-scaled). */

static int soft;                                 /* headless: this path */
static uint8_t soft_idx[SCREEN_W * SCREEN_H];
static uint32_t soft_pal[256], soft_rgb[SCREEN_W * SCREEN_H];
static int soft_is_rgb;

static inline int present_headless(void)
{
	return getenv("NANPA2_HEADLESS") != NULL;
}

/* the last frame as 0x00RRGGBB in soft_rgb */
static inline void soft_convert(void)
{
	if (!soft_is_rgb)
		for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
			soft_rgb[i] = soft_pal[soft_idx[i]];
}

static inline void soft_probe(void)
{
	uint32_t now = SDL_GetTicks();
	if (pstats.last_probe && now - pstats.last_probe < 1000)
		return;
	soft_convert();
	present_note_probe(present_luma_rgb(soft_rgb));
}

static inline void soft_frame(const uint8_t *pixels, const uint32_t pal[256])
{
	memcpy(soft_idx, pixels, sizeof soft_idx);
	memcpy(soft_pal, pal, sizeof soft_pal);
	soft_is_rgb = 0;
	present_note_in(present_luma_indexed(pixels, pal));
	soft_probe();
}

static inline void soft_frame_rgb(const uint32_t *xrgb)
{
	memcpy(soft_rgb, xrgb, sizeof soft_rgb);
	soft_is_rgb = 1;
	present_note_in(present_luma_rgb(xrgb));
	soft_probe();
}

static inline int soft_capture(const char *bmp_path, int w, int h)
{
	if (w <= 0 || h <= 0) {
		w = SCREEN_W;
		h = SCREEN_H;
	}
	soft_convert();
	SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_XRGB8888);
	if (!s)
		return 0;
	for (int y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)((uint8_t *)s->pixels + y * s->pitch);
		const uint32_t *src = soft_rgb + (y * SCREEN_H / h) * SCREEN_W;
		for (int x = 0; x < w; x++)
			row[x] = src[x * SCREEN_W / w];
	}
	int ok = SDL_SaveBMP(s, bmp_path);
	SDL_DestroySurface(s);
	return ok;
}

/* ---- display mode state (present.h) ---- */

static int disp_mode = DISPLAY_CRT_HD, disp_lines = 1080, disp_smooth;
static void (*disp_cb)(int);
static SDL_Window *disp_win;
static char disp_title[160];
static Uint32 disp_title_until;

static const char *const disp_names[DISPLAY_MODES] = { "CRT HD", "CRT 480", "Sharp", "Smooth" };

const char *present_mode_name(int mode)
{
	return mode >= 0 && mode < DISPLAY_MODES ? disp_names[mode] : "?";
}

int present_parse_mode(const char *s)
{
	static const char *const k[DISPLAY_MODES] = { "hd", "480", "sharp", "smooth" };
	for (int i = 0; s && i < DISPLAY_MODES; i++)
		if (!strcmp(s, k[i]))
			return i;
	return -1;
}

void present_set_mode(int mode)
{
	if (mode >= 0 && mode < DISPLAY_MODES)
		disp_mode = mode;
}

int present_get_mode(void)
{
	return disp_mode;
}

void present_set_crt_lines(int lines)
{
	disp_lines = lines < 600 ? 600 : lines > 1600 ? 1600 : lines;
}

void present_set_crt_scaler(int smooth)
{
	disp_smooth = smooth != 0;
}

void present_on_mode_change(void (*cb)(int mode))
{
	disp_cb = cb;
}

void present_set_crt(int on)
{
	disp_mode = on ? (disp_mode <= DISPLAY_CRT_480 ? disp_mode : DISPLAY_CRT_HD) : DISPLAY_SHARP;
}

int present_get_crt(void)
{
	return disp_mode <= DISPLAY_CRT_480;
}

/* the presenter's init gives its window, for the mode name in the title */
static inline void present_common_init(SDL_Window *win)
{
	disp_win = win;
}

/* every presented frame: put the title back after the mode name's moment */
static inline void present_common_tick(void)
{
	if (disp_title_until && (Sint32)(SDL_GetTicks() - disp_title_until) >= 0) {
		disp_title_until = 0;
		if (disp_win)
			SDL_SetWindowTitle(disp_win, disp_title);
	}
}

/* the window title carries a note for two seconds (the mode's name after F12) */
static void present_title_note(const char *note)
{
	if (!disp_win)
		return;
	if (!disp_title_until)
		snprintf(disp_title, sizeof disp_title, "%s", SDL_GetWindowTitle(disp_win));
	char t[200];
	snprintf(t, sizeof t, "%s - %s", disp_title, note);
	SDL_SetWindowTitle(disp_win, t);
	disp_title_until = SDL_GetTicks() + 2000;
	if (!disp_title_until)
		disp_title_until = 1;
}

void present_cycle_mode(void)
{
	disp_mode = (disp_mode + 1) % DISPLAY_MODES;
	present_title_note(disp_names[disp_mode]);
	if (disp_cb)
		disp_cb(disp_mode);
}

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
	SDL_SetWindowFullscreen(win, on != 0);
}

static inline int present_common_get_fullscreen(SDL_Window *win)
{
	return (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) != 0;
}

/* Shared hotkey handling (see present_hotkey). The key that completes a
 * chord is remembered so that its release is swallowed too, even if the
 * modifier was let go first. */
static inline int present_common_hotkey(SDL_Window *win, const SDL_KeyboardEvent *k)
{
	static SDL_Keycode held;
	SDL_Keycode sym = k->key;
	Uint16 mod = k->mod;
	if (k->type == SDL_EVENT_KEY_UP) {
		if (sym == SDLK_F12)
			return 1;
		if (held && sym == held) {
			held = 0;
			return 1;
		}
		return 0;
	}
	if (sym == SDLK_F12) {
		if (!k->repeat) {
			present_cycle_mode();
		}
		return 1;
	}
	int alt_enter = (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) && (mod & SDL_KMOD_ALT);
#ifdef __APPLE__
	int cmd_f = sym == SDLK_F && (mod & SDL_KMOD_GUI);
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
