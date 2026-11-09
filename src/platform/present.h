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

/* A frame below this mean luminance (0..255) counts as black. */
#define PRESENT_BLACK_LUMA 4

/* Presenter counters for the live status file and watchdog (src/dev/status.c).
 * Call on the main thread. Times are SDL_GetTicks() values, 0 = never. */
struct present_stats {
	unsigned long frames_in;      /* present_frame / present_frame_rgb calls */
	unsigned long presented;      /* frames that reached the window */
	unsigned long skip_occluded;  /* skipped: the window is covered or hidden */
	unsigned long skip_drawable;  /* skipped: no drawable (nextDrawable nil) */
	unsigned long skip_size;      /* skipped: drawable size 0 */
	double last_wait_ms;          /* time to get the last drawable */
	double worst_wait_ms;         /* longest such wait */
	int drawable_w, drawable_h;
	int occluded;                 /* 1 covered, 0 visible, -1 unknown */
	int os_windows;               /* this process's on-screen windows, -1 unknown */
	int luma_in;                  /* mean luminance 0..255 of the last frame given */
	int luma_shown;               /* ... of the last frame presented, -1 none */
	uint32_t last_present;        /* when a frame last reached the window */
	uint32_t last_nonblack;       /* when a non-black frame last reached it */
	int app_hidden;               /* the application is hidden (Cmd-H), -1 unknown */
	/* Offscreen probe: while presents are skipped because the window is
	 * covered (or the screen locked), the presenter renders the frame it
	 * skipped through the same pipeline into an offscreen texture about
	 * once a second and reads it back, so the status file shows whether
	 * rendering works although nothing can reach the screen. The probe
	 * runs with the CRT simulation off (its dark glass would make a black
	 * frame read as grey) and does not touch the counters above. */
	unsigned long probes;         /* offscreen renders done */
	int luma_probe;               /* mean luminance of the last one, -1 none */
	int luma_probe_src;           /* ... of the frame it was given (luma_in then) */
	uint32_t last_probe;          /* when the last one ran */
	uint32_t last_probe_nonblack; /* when one last came out non-black */
};
void present_stats(struct present_stats *out);

#endif
