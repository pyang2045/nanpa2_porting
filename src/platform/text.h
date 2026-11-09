/*
 * Platform API: text (R4, docs/refactor.md "The platform API").
 *
 * The game draws one character at a time, in a Big5 TrueType face at three sizes, as GDI's
 * TextOut did on an 8-bit device context. The platform finds a CJK font on the host (--font, or
 * the usual places), prefers a face's embedded bitmap strike for the size and otherwise
 * rasterises the outline unantialiased with GDI's emboldening. The drawing costs no game time;
 * the game charges the original's (src/game/pace.h).
 */
#ifndef NANPA2_PLATFORM_TEXT_H
#define NANPA2_PLATFORM_TEXT_H

#include <stdint.h>

/* Find the font: font_path (may be NULL) or a candidate of the host. 0 if there is none (text
 * is then blank). */
int text_init(const char *font_path);

/* A font of em height `height` (negative: character height as in GDI) and weight; a handle */
uint32_t text_font_create(int height, int weight);

/* Draw the Big5 (2 bytes) or ASCII (1 byte) character in str, len bytes, at (x, y) of the surface
 * in the font: colours are nonzero or zero, opaque fills the character's cell with the
 * background first. */
void text_draw(uint32_t surface, int x, int y, uint32_t font, const uint8_t *str, int len, uint32_t fg_colour,
	       uint32_t bg_colour, int opaque);

#endif
