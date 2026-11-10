/*
 * Script text (stage 4): drawing one character 0x401900, the text cursor
 * 0x4063ee (with the newline 0x406415) and clearing the text window
 * 0x406b24.
 *
 * The text window is a rectangle in 8-pixel columns (x) and pixel rows
 * (y) on surface 0; the cursor advances by a character width in columns
 * and wraps to the next line, and from the last line back to the top.
 * A character is drawn into the scratch surface G.sc.scratch_surface (through
 * the platform's text call, src/platform/text.c) and copied
 * from there as an opaque N x N cell in the text colours. Semantics:
 * re-notes.md "Text routine".
 */
#include <stddef.h>

#include "game/ctl.h"
#include "game/pace.h"
#include "game/vm.h"
#include "game/gmem.h"
#include "platform/surface.h"
#include "platform/text.h"


#define SCRATCH_PITCH 0x280u

/* 0x401900: draw character c (Big5 if bit 15 is set, high byte first; else
 * ASCII) with its top left at pixel (x, y) of the screen in font size & 3,
 * as an opaque cell of N x N pixels, N = 8 * (size & 3) + 16: fg where the
 * glyph is, bg elsewhere (the low bytes of the colours are stored). The
 * cell is copied from the scratch surface, whose rows are cleared again (32
 * bytes each) on the way. The glyph is drawn by the platform's text call into the
 * scratch surface (white on black, opaque, at (0, 0)); the pacing model charges
 * the cell copy and, for the original's GDI TextOut, a time a character. */
void game_draw_char(uint16_t c, uint32_t x, uint32_t y, uint32_t size, uint8_t fg, uint8_t bg)
{
	ctl_tlog("ch", c);
	uint8_t str[2];
	uint32_t len;
	if (c & 0x8000) {
		str[0] = (uint8_t)(c >> 8);
		str[1] = (uint8_t)c;
		len = 2;
	} else {
		str[0] = (uint8_t)c;
		len = 1;
	}
	size &= 3;
	uint32_t n = 8 * size + 16;
	ctl_work_us(PACE_CELL_US(n));
	uint32_t scratch = SC(scratch_surface);

	/* the glyph, white on black, at (0, 0) of the scratch surface */
	ctl_work_us(PACE_GLYPH_US * len);
	text_draw(scratch, 0, 0, SC(font[size]), str, (int)len, 0xf0f0f0, 0, 1);

	/* the cell, from the scratch surface to the screen */
	uint32_t screen = G.rt.surface[0];
	uint8_t *s = game_surface_lock(scratch);
	uint8_t *d = game_surface_lock(screen) + G.rt.pitch[0] * y + x;
	uint8_t *d0 = d;
	for (uint32_t row = 0; row < n; row++) {
		for (uint32_t col = 0; col < n; col++) {
			int on = s[col] != 0;
			d[col] = on ? fg : bg;
		}
		memset(s, 0, 32);
		s += SCRATCH_PITCH;
		d += G.rt.pitch[0];
	}
	game_surface_unlock(scratch);
	/* the platform's text layer is told where the cell went (nothing of the game changes) */
	int pitch;
	ptrdiff_t off = d0 - surf_lock(screen, &pitch);
	if (off >= 0 && pitch > 0)
		text_cell_placed(screen, (int)(off % pitch), (int)(off / pitch), (int)n, fg, bg);
	game_surface_unlock(G.rt.surface[0]);
}

/* 0x406415: the cursor to the start of the next line, or of the window's
 * first line when the next one would not fit. */
void game_text_newline(void)
{
	uint32_t h = SV(text_line);
	SV_SET(text_x, SV(text_left));
	uint32_t y = (SV(text_y) + h) & 0xffff;
	SV_SET(text_y, y);
	if (((y + h - 2) & 0xffff) < SV(text_bottom)) {
		return;
	}
	SV_SET(text_y, SV(text_top));
}

/* 0x4063ee: the cursor past one character, to the next line when the
 * next one would not fit. */
void game_text_advance(void)
{
	uint32_t w = SV(text_advance);
	uint32_t x = (SV(text_x) + w) & 0xffff;
	SV_SET(text_x, x);
	if (((x + w - 2) & 0xffff) < SV(text_right)) {
		return;
	}
	game_text_newline();
}

/* 0x406b24: fill the text window on surface @[6] with the colour (the fill
 * routine's colour << 4: the caller's EBX) and home the cursor. The fill's
 * arguments are built in AX over EAX's upper half as natively, so the
 * rectangle words carry the bits `shl eax, 3` shifts out of the low half. */
void game_clear_text_window(uint32_t colour)
{
	ctl_tlog("clr", colour);
	uint32_t a = SV(surface_select), surf = a;
	a = (a & 0xffff0000u) | ((SV(text_bottom) - SV(text_top) + 1) & 0xffff);
	uint32_t h = a;
	a = ((a & 0xffff0000u) | ((SV(text_right) - SV(text_left) + 1) & 0xffff)) << 3;
	uint32_t w = a;
	uint32_t top = SV(text_top), left = SV(text_left);
	a = (a & 0xffff0000u) | top;
	SV_SET(text_y, top);
	uint32_t y = a;
	a = ((a & 0xffff0000u) | left) << 3;
	SV_SET(text_x, left);
	uint32_t x = a;
	game_fill_rect(x, y, w, h, colour, surf);
}
