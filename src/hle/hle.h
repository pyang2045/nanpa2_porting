/* The host layer (dev tools, replay, clock, status, text, window): declarations shared by its modules. */
#ifndef NANPA2_HLE_H
#define NANPA2_HLE_H

#include <stddef.h>
#include <stdint.h>

#include "game/gmem.h"
#include "platform/window.h"

/* Dev probes (win32.c): the script VM fetched the opcode at offset off of
 * segment 0; the hit test (04 23) is about to scan the rectangle table at
 * script address table. They feed NANPA2_VMTRACE, the exploration bot and
 * NANPA2_HOTSPOTS. */
void probe_vm_fetch(uint32_t off);
void probe_hit_table(uint32_t table);

/* Setup: the folders the game files come from. */
int hle_init(const char *game_dir, const char *save_dir);

/* ---- modules ---- */

/* win32.c: window, messages, timer, files */
void win32_post_message(uint32_t msg, uint32_t wparam, uint32_t lparam);
/* the same for a message from outside the game (host, scripted input, the bot):
 * recorded by --record, dropped while replaying (replay.h) */
void win32_post_ext(uint32_t msg, uint32_t wparam, uint32_t lparam);
uint32_t win32_timer_ticks(void); /* timer callbacks run so far */
void win32_run_timer(void);  /* run due 16 ms ticks (native timer thread) */
int win32_timer_running(void); /* timeSetEvent seen, not killed */
/* Cursor. The native game hides the system cursor (ShowCursor counter -1:
 * 0x401e27, 0x40262a, 0x4028ab) and draws its own sprite from
 * DATA/MOUSE.DAT; the host cursor is hidden only while the counter is
 * negative and the pointer is over the game image of the focused window. */
void win32_cursor_update(void);  /* re-check after mouse/window/fit changes */
void win32_cursor_state(int *count, int *shown, int *over_image);

/* platform/surface.h: surfaces and the screen */
#include "platform/surface.h"

/* text.c: text into surfaces */
int text_init(const char *font_path);
/* A font of em height `height` (negative: character height as in GDI) and weight; a handle */
uint32_t text_font_create(int height, int weight);
/* Draw the Big5 (2 bytes) or ASCII (1 byte) character in str, len bytes, at (x, y) of the surface
 * in the font, as GDI's TextOut did on an 8-bit device context: colours are nonzero or zero,
 * opaque fills the character's cell with the background first. */
void text_draw(uint32_t surface, int x, int y, uint32_t font, const uint8_t *str, int len, uint32_t fg_colour,
	       uint32_t bg_colour, int opaque);
/* sound.c: see platform/sound.h */
#include "platform/sound.h"

#endif
