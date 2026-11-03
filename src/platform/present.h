#ifndef NANPA2_PRESENT_H
#define NANPA2_PRESENT_H

#include <SDL.h>
#include <stdint.h>

/* The game draws into a 640x480 8-bit screen with a 256-entry palette
 * (as the native DirectDraw primary surface). A presenter shows that screen
 * in the window, scaled to fit with letterboxing.
 * macOS: CAMetalLayer (present_metal.m). Elsewhere: SDL_Renderer. */

#define SCREEN_W 640
#define SCREEN_H 480

/* Window flags the presenter needs (e.g. SDL_WINDOW_METAL). */
Uint32 present_window_flags(void);
int present_init(SDL_Window *win);
/* pixels: SCREEN_W*SCREEN_H palette indices; pal: 0x00RRGGBB */
void present_frame(const uint8_t *pixels, const uint32_t pal[256]);
const char *present_name(void);
void present_shutdown(void);

/* Map a window point to screen coordinates; returns 0 if outside.
 * Follows the CRT curvature when that is on. */
int present_window_to_screen(int wx, int wy, int *sx, int *sy);

/* CRT monitor simulation (curvature, scanlines, shadow mask, vignette).
 * On by default; F12 toggles it. SDL_Renderer only draws scanlines. */
void present_set_crt(int on);
int present_get_crt(void);

/* Render the last frame at w x h and save it as a BMP (for checking the
 * presenter headlessly). Returns 0 on failure. */
int present_capture(const char *bmp_path, int w, int h);

#endif
