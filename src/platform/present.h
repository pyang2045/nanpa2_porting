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
/* A true-colour frame (the opening movie, which the native code shows in a
 * 640x480x16 mode): SCREEN_W*SCREEN_H pixels 0x00RRGGBB. */
void present_frame_rgb(const uint32_t *xrgb);
const char *present_name(void);
void present_shutdown(void);

/* Map a window point to screen coordinates; returns 0 if outside. */
int present_window_to_screen(int wx, int wy, int *sx, int *sy);

/* Screen pixel to window (point) coordinates: the inverse of
 * present_window_to_screen, for warping the mouse (SetCursorPos). */
void present_screen_to_window(int sx, int sy, int *wx, int *wy);

/* Port hotkeys: F12 toggles the CRT simulation, Alt+Enter (and Cmd+F on
 * macOS) toggles fullscreen. Returns 1 if the key event (down, up or
 * repeat) belongs to one of them, so the caller does not pass it on. */
int present_hotkey(const SDL_KeyboardEvent *key);
void present_set_fullscreen(int on);
int present_get_fullscreen(void);

/* CRT monitor simulation: a flat Sony FD Trinitron style tube (scanlines,
 * aperture grille, damper wires, glow, dark glass), in the window and in
 * fullscreen alike, at any size. On by default. */
void present_set_crt(int on);
int present_get_crt(void);

/* Render the last frame at w x h and save it as a BMP (for checking the
 * presenter headlessly). Returns 0 on failure. */
int present_capture(const char *bmp_path, int w, int h);

#endif
