/*
 * Platform API: surfaces, the palette and the screen (R4, docs/refactor.md "The platform API").
 *
 * The game draws into 8-bit surfaces held by the platform: the screen (the primary surface,
 * 640x480, what the window shows), three off-screen 640x400 work surfaces and a 640x480 scratch
 * surface for text. A surface is named by an opaque handle the game keeps in its tables; the
 * pixels are plain memory that a lock hands out as a pointer, valid for the life of the surface
 * (there is no real lock: the pointer does not change, and a screen shows what is in it the next
 * time the window is presented).
 *
 * A surface's memory has slack around it (SURF_SLACK bytes each side), because the game's
 * rectangles come from scripts unchecked and the original ran over the end of a surface into
 * whatever lay behind it; here such a write lands in the slack.
 *
 * The palette is 256 entries of {red, green, blue, flags}, the original's PALETTEENTRY.
 * Drawing operations here cost no game time: the game charges its own (src/game/pace.h).
 */
#ifndef NANPA2_PLATFORM_SURFACE_H
#define NANPA2_PLATFORM_SURFACE_H

#include <stdint.h>

#define SURF_SLACK (1u << 20)

/* A new surface of w x h pixels, zeroed; `primary` makes it the screen (w x h are then the
 * display's). Returns its handle, 0 when there is no room. The primary's handle is a fixed
 * value (SURF_PRIMARY_HANDLE): the script register S receives the last unlock's result, which
 * is the screen's handle after an off-screen surface was unlocked, and a recording carries it. */
#define SURF_PRIMARY_HANDLE 0x0104b098u
uint32_t surf_create(int w, int h, int primary);

/* The pixels of the surface and its pitch (bytes a row); NULL for an unknown handle. */
uint8_t *surf_lock(uint32_t surface, int *pitch);
/* The width and height of the surface; 0 for an unknown handle. */
int surf_size(uint32_t surface, int *w, int *h);

/* The source colour key (pixel values lo..hi are transparent in a keyed copy), or none. */
void surf_set_colorkey(uint32_t surface, int enabled, uint32_t lo, uint32_t hi);
int surf_has_colorkey(uint32_t surface);

/* Copy the rectangle [l, r) x [t, b) of src to (x, y) of dst, pixel by pixel with the clipping
 * of the original DirectDraw BltFast (a pixel outside either surface is skipped); keyed: skip
 * the source pixels in src's colour key. */
void surf_blt_fast(uint32_t dst, int x, int y, uint32_t src, int l, int t, int r, int b, int keyed);

/* Fill the rectangle [l, r) x [t, b) of the surface (clipped to it) with the low byte of colour. */
void surf_fill(uint32_t surface, int l, int t, int r, int b, uint32_t colour);

/* Palette entries start..start+count-1 from entries (4 bytes each). */
void pal_set(uint32_t start, uint32_t count, const uint8_t *entries);

/* The screen as 640x480 palette indices and the palette as 0x00RRGGBB; 0 if there is no screen. */
int screen_snapshot(uint8_t *frame, uint32_t pal[256]);
/* Show the screen in the window (at most every 16 ms unless forced). */
void screen_present(int force);
/* Wait for the vertical blank: run the due timer ticks and present. */
void screen_wait_vblank(void);

#endif
