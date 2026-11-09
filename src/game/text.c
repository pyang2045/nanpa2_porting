/*
 * Script text (stage 4): drawing one character 0x401900, the text cursor
 * 0x4063ee (with the newline 0x406415) and clearing the text window
 * 0x406b24.
 *
 * The text window is a rectangle in 8-pixel columns (x) and pixel rows
 * (y) on surface 0; the cursor advances by a character width in columns
 * and wraps to the next line, and from the last line back to the top.
 * A character is drawn by GDI into the scratch surface 0x4ab058 (through
 * the game's own import calls, which src/hle/text.c serves) and copied
 * from there as an opaque N x N cell in the text colours. Semantics:
 * re-notes.md "Text routine".
 */
#include "game/ctl.h"
#include "game/vm.h"
#include "x86/x86.h"
#include "game/state.h"


#define SURFACES 0x44629eu     /* u32[]: surface objects, [0] the screen */
#define PITCHES 0x4462aeu      /* u32[]: their pitches */
#define SCRATCH_PITCH 0x280u
#define FONTS 0x4ac070u        /* u32[4]: font handles by size */

/* imports and COM methods the native code calls (IAT slots, vtable
 * offsets) */
#define IAT_TEXTOUT 0x41b014u
#define IAT_SELECTOBJECT 0x41b020u
#define IAT_SETTEXTCOLOR 0x41b024u
#define IAT_SETBKCOLOR 0x41b028u
#define IAT_SETBKMODE 0x41b02cu
#define SURFACE_GETDC 0x44u
#define SURFACE_RELEASEDC 0x68u
#define BK_OPAQUE 2u

/* 0x401900: draw character c (Big5 if bit 15 is set, high byte first; else
 * ASCII) with its top left at pixel (x, y) of the screen in font size & 3,
 * as an opaque cell of N x N pixels, N = 8 * (size & 3) + 16: fg where the
 * glyph is, bg elsewhere (the low bytes of the colours are stored). The
 * cell is copied from the scratch surface, whose rows are cleared again (32
 * bytes each) on the way. The character string and the device context the
 * GDI calls use live in two words of the emulated stack (the HLE boundary,
 * until R4). */
void game_draw_char(uint16_t c, uint32_t x, uint32_t y, uint32_t size, uint8_t fg, uint8_t bg)
{
	ctl_tlog("ch", c);
	uint32_t f = game_frame_alloc(8);
	uint32_t str = f + 4, dc_at = f;
	uint32_t len;
	if (c & 0x8000) {
		wr8(str, c >> 8);
		wr8(str + 1, c);
		wr8(str + 2, 0);
		len = 2;
		COST(14);
	} else {
		wr8(str, c);
		wr8(str + 1, 0);
		len = 1;
		COST(10);
	}
	size &= 3;
	uint32_t n = 8 * size + 16;
	uint32_t scratch = SC(scratch_surface), vtbl = rd32(scratch);

	/* the glyph, white on black, at (0, 0) of the scratch surface */
	COST(9);
	HLE(rd32(vtbl + SURFACE_GETDC), scratch, dc_at);
	uint32_t dc = rd32(dc_at);
	COST(5);
	HLE(rd32(IAT_SELECTOBJECT), dc, rd32(FONTS + 4 * size));
	COST(4);
	HLE(rd32(IAT_SETTEXTCOLOR), dc, 0xf0f0f0);
	COST(4);
	HLE(rd32(IAT_SETBKCOLOR), dc, 0);
	COST(4);
	HLE(rd32(IAT_SETBKMODE), dc, BK_OPAQUE);
	COST(8);
	HLE(rd32(IAT_TEXTOUT), dc, 0, 0, str, len);
	COST(6);
	HLE(rd32(vtbl + SURFACE_RELEASEDC), scratch, dc);

	/* the cell, from the scratch surface to the screen */
	uint32_t screen = G.rt.surface[0];
	COST(3);
	uint32_t src = game_surface_lock(scratch);
	COST(4);
	uint32_t dst = game_surface_lock(screen) + G.rt.pitch[0] * y + x;
	COST(10);
	for (uint32_t row = 0; row < n; row++) {
		COST(4);
		uint8_t *s = x86_ptr(src, 32 > n ? 32 : n), *d = x86_ptr(dst, n);
		for (uint32_t col = 0; col < n; col++) {
			int on = s[col] != 0;
			d[col] = on ? fg : bg;
			COST(on ? 7 : 8);
			if (col + 1 < n)
				POLL(0x401a07);
		}
		memset(s, 0, 32);
		src += SCRATCH_PITCH;
		dst += G.rt.pitch[0];
		COST(11 + 8);
		if (row + 1 < n)
			POLL(0x401a2b);
	}
	COST(3);
	game_surface_unlock(scratch);
	COST(3);
	game_surface_unlock(G.rt.surface[0]);
	COST(7);
	game_frame_free(8);
}

/* 0x406415: the cursor to the start of the next line, or of the window's
 * first line when the next one would not fit. */
void game_text_newline(void)
{
	uint32_t h = SV(text_line);
	SV_SET(text_x, SV(text_left));
	uint32_t y = (SV(text_y) + h) & 0xffff;
	SV_SET(text_y, y);
	COST(10);
	if (((y + h - 2) & 0xffff) < SV(text_bottom)) {
		COST(1);
		return;
	}
	SV_SET(text_y, SV(text_top));
	COST(3);
}

/* 0x4063ee: the cursor past one character, to the next line when the
 * next one would not fit. */
void game_text_advance(void)
{
	uint32_t w = SV(text_advance);
	uint32_t x = (SV(text_x) + w) & 0xffff;
	SV_SET(text_x, x);
	COST(8);
	if (((x + w - 2) & 0xffff) < SV(text_right)) {
		COST(1);
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
	COST(20);
	game_fill_rect(x, y, w, h, colour, surf);
	COST(2);
}
