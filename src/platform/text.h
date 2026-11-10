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

#include "platform/textlayer.h"

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

/* The game copied the cell of the last text_draw to (x, y) of the surface with the colour indices
 * fg and bg (see platform/textlayer.h): the text layer follows what is on the screen. Touches
 * nothing of the game. */
void text_cell_placed(uint32_t surface, int x, int y, int n, uint8_t fg, uint8_t bg);

/* An antialiased bitmap of the character the text layer recorded (textlayer.h, struct
 * tl_glyph), `scale` output pixels per game pixel; the platform's, see text.c. */
/* The font face number `face` text.c loaded: its file image and the face's offset in it (0: none). */
int text_face_font(int face, const unsigned char **data, int *offset);
uint8_t *text_hires_glyph(const struct tl_glyph *g, float scale, int *w, int *h, int *xo, int *yo, float *pen_x);
void text_hires_free(uint8_t *bm);

#endif
