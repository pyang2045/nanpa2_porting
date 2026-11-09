/*
 * The palette (stage 5B): the two colour tables, the DirectDraw palette
 * entries built from them, the fade between the displayed entries and the
 * tables, and the wait for the fade to end.
 *
 * The game keeps its colours in two tables (PAL_BASE: 16 u16 in 4:4:4, the
 * interface colours; PAL_PICTURE: 240 u16 in 5:5:5, the picture's) and
 * builds 256 PALETTEENTRY records at PAL_ENTRIES from them. A fade
 * (0x401d30) stores, per entry and channel, the distance from the entries
 * displayed now to the tables, and the start colours at PAL_START; the
 * 16 ms timer callback (src/hle/hle.c, native 0x4028c0) then moves the
 * entries 16 steps along it, and clears PAL_FADING at the end.
 * Semantics: re-notes.md "Palette and fades".
 *
 * Time costs are the few of pace.h (the fade set-up); the drawing routines cost none.
 */
#include "game/ctl.h"
#include "game/pace.h"
#include "game/file.h"
#include "game/vm.h"
#include "game/gmem.h"
#include "game/state.h"
#include "game/gmem.h"
#include "platform/sound.h"
#include "platform/surface.h"
#include "platform/text.h"
#include "platform/window.h"
#include "platform/surface.h"

#define PAL_BASE_N 16u
#define PAL_PICTURE_N 240u
/* The tables (state.h): G.save.n.palette[16] 4:4:4 (0x0GRB, one nibble per channel), G.rt.palette_picture[240]
 * 5:5:5 (red in bits 10-14), G.rt.palette_saved[256] the same 16 + 240 colours saved (0x40756c),
 * G.sc.pal_entries PALETTEENTRY[256] (red, green, blue, flags), G.sc.pal_delta[256][3] target - displayed,
 * G.sc.pal_start the entries as the fade started. */

/* 0x40756c / 0x407599: save the tables to PAL_SAVED, or put them back,
 * unless @[9] bit 4 is set. */
static void palette_copy_tables(int save)
{
	if (SV(vflags) & 0x10) {
		return;
	}
	if (save) {
		memcpy(G.rt.palette_saved, G.save.n.palette, sizeof G.save.n.palette);
		memcpy(G.rt.palette_saved + PAL_BASE_N, G.rt.palette_picture, sizeof G.rt.palette_picture);
	} else {
		memcpy(G.save.n.palette, G.rt.palette_saved, sizeof G.save.n.palette);
		memcpy(G.rt.palette_picture, G.rt.palette_saved + PAL_BASE_N, sizeof G.rt.palette_picture);
	}
}

void game_palette_save(void)    /* 0x40756c */
{
	palette_copy_tables(1);
}

void game_palette_restore(void) /* 0x407599 */
{
	palette_copy_tables(0);
}

/* 0x40754d: all 16 + 240 colours = v (only the low word of the argument
 * is stored). */
void game_palette_fill(uint16_t v)
{
	for (uint32_t i = 0; i < PAL_BASE_N; i++)
		G.save.n.palette[i] = v;
	for (uint32_t i = 0; i < PAL_PICTURE_N; i++)
		G.rt.palette_picture[i] = v;
}

/* 0x401ca0: build the 256 entries from the tables and give them to the
 * DirectDraw palette (SetEntries(0, 0, 256, entries)). */
void game_palette_upload(void)
{
	for (uint32_t i = 0; i < PAL_BASE_N; i++) {
		uint32_t w = G.save.n.palette[i];
		uint8_t *e = G.sc.pal_entries + 4 * i;
		e[0] = w & 0xf0;
		e[1] = (w >> 4) & 0xf0;
		e[2] = (w & 0x0f) << 4;
	}
	for (uint32_t i = 0; i < PAL_PICTURE_N; i++) {
		uint32_t w = G.rt.palette_picture[i];
		uint8_t *e = G.sc.pal_entries + 4 * (PAL_BASE_N + i);
		e[0] = (w >> 7) & 0xf8;
		e[1] = (w >> 2) & 0xf8;
		e[2] = (w & 0x1f) << 3;
	}
	pal_set(0, 256, G.sc.pal_entries);
}

/* 0x401d30: start a fade from the displayed entries to the tables. Returns
 * EAX as the routine leaves it, the last blue distance: op 19 2 0 stores it
 * as the script's S. */
uint32_t game_fade_start(void)
{
	uint32_t last = 0;
	for (uint32_t i = 0; i < PAL_BASE_N; i++) {
		uint32_t w = G.save.n.palette[i];
		const uint8_t *e = G.sc.pal_entries + 4 * i;
		G.sc.pal_delta[i][0] = (int32_t)((w & 0xf0) - e[0]);
		G.sc.pal_delta[i][1] = (int32_t)(((w >> 4) & 0xf0) - e[1]);
		G.sc.pal_delta[i][2] = (int32_t)(((w & 0x0f) << 4) - e[2]);
	}
	for (uint32_t i = 0; i < PAL_PICTURE_N; i++) {
		uint32_t w = G.rt.palette_picture[i];
		const uint8_t *e = G.sc.pal_entries + 4 * (PAL_BASE_N + i);
		int32_t (*d)[3] = &G.sc.pal_delta[PAL_BASE_N + i];
		(*d)[0] = (int32_t)(((w >> 7) & 0xf8) - e[0]);
		(*d)[1] = (int32_t)(((w >> 2) & 0xf8) - e[1]);
		(*d)[2] = (int32_t)(((w & 0x1f) << 3) - e[2]);
		last = ((w & 0x1f) << 3) - e[2];
	}
	memcpy(G.sc.pal_start, G.sc.pal_entries, sizeof G.sc.pal_start);
	SC_SET(fade_active, 1);
	ctl_tlog("fade+", 0);
	ctl_work_us(PACE_FADE_US);
	return last;
}

/* 0x401e10: whether a fade runs (the timer callback clears it) */
uint32_t game_fade_active(void)
{
	return SC(fade_active);
}

/* 0x406f41: op 04 16 3 waits for the fade: the native code asks again until
 * the timer callback has cleared the flag, spinning (no pump); then it puts
 * the tables back. */
void game_palette_wait(void)
{
	for (;;) {
		uint32_t active = game_fade_active();
		if (!active)
			break;
		ctl_work_us(PACE_SPIN_US);
	}
	game_palette_restore();
}

/*
 * Surfaces and the game cursor.
 *
 * The screen is surface 0 of G.rt.surface (platform surface handles, pitches in G.rt.pitch);
 * the scratch surface G.sc.scratch_surface and the rest are numbered after it. Every routine
 * that draws on or locks the screen first takes the game's own cursor off it and puts it back
 * afterwards: the cursor is a 32x32 sprite (two bit planes per frame of G.cursor_bits: where
 * the second plane is clear the pixel turns black, where the first is set it turns white) drawn
 * straight into the locked screen, with the pixels under it kept in G.sc.cursor_saved so that
 * they can be put back. When G.sc.letterbox is set the picture is drawn 40 lines below the
 * surface's top, and every rectangle on surface 0 moves with it. Semantics: re-notes.md
 * "Surfaces and cursor".
 *
 * A lock hands out the surface's pixel pointer (src/platform/surface.h). The coordinates the
 * scripts give are not checked, as natively: a rectangle that leaves a surface lands in the
 * platform's slack around it, or (blits) is clipped by the platform.
 */
#define SURFACES 0x44629eu    /* u32[]: surface handles, [0] the screen */
#define PITCHES 0x4462aeu     /* u32[]: their pitches */
/* G.sc.cursor_saved: 32 rows of 32 bytes; G.cursor_bits: the two planes (AND, XOR) of each frame, 0x100 apart, as
 * big-endian dwords per row. A frame number past the table reads on into the data that follows, as natively. */
static uint32_t cursor_plane(uint32_t off)
{
	uint32_t v;
	memcpy(&v, (uint8_t *)&G.cursor_bits + off, 4);
	return v;
}
#define SHIFT_LINES 0x28u

/* the surface handle of surface i (the index comes from the script unchecked: past 3 it reads
 * on into the pitch table and the segment after, which name no surface) and its pitch */
static uint32_t surface_at(uint32_t i)
{
	return rd32(SURFACES + 4 * i);
}

static uint32_t pitch_at(uint32_t i)
{
	return rd32(PITCHES + 4 * i);
}

/* 0x4011f0: put the pixels under the cursor back (the width and height
 * clip at the screen's edge). */
void game_cursor_restore(void)
{
	uint32_t x = SC(cursor_saved_x), y = SC(cursor_saved_y), w, h;
	if ((int32_t)x > 0x260) {
		w = 0x280 - x;
	} else {
		w = 0x20;
	}
	if ((int32_t)y > 0x1c0) {
		h = 0x1e0 - y;
	} else {
		h = 0x20;
	}
	uint32_t screen = G.rt.surface[0];
	uint32_t pitch = G.rt.pitch[0];
	uint8_t *row = surf_lock(screen, NULL) + pitch * y + x;
	if ((int32_t)h > 0) {
		for (uint32_t r = 0; r < h; r++) {
			memcpy(row, G.sc.cursor_saved + 32 * r, w);
			row += pitch;
		}
	}
}

/* 0x4012e0: save the pixels under the cursor and draw it at its place.
 * Returns EAX as the routine leaves it, the cursor's x. */
uint32_t game_cursor_draw(void)
{
	uint32_t x = SC(cursor_x), y = SC(cursor_y), w, h;
	if ((int32_t)x > 0x260) {
		w = 0x280 - x;
	} else {
		w = 0x20;
	}
	if ((int32_t)y > 0x1c0) {
		h = 0x1e0 - y;
	} else {
		h = 0x20;
	}
	uint32_t frame = SC(cursor_frame);
	uint32_t plane1 = frame << 8, plane2 = 0x80 + (frame << 8);
	uint32_t screen = G.rt.surface[0];
	uint32_t pitch = G.rt.pitch[0];
	uint8_t *top = surf_lock(screen, NULL) + pitch * y + x;
	if ((int32_t)h > 0) {
		uint8_t *row = top;
		for (uint32_t r = 0; r < h; r++) {
			memcpy(G.sc.cursor_saved + 32 * r, row, w);
			row += pitch;
		}
		row = top;
		for (uint32_t r = 0; r < h; r++) {
			uint32_t bit = 0x80000000u;
			uint8_t *px = row;
			for (uint32_t c = 0; (int32_t)w > 0 && c < w; c++, bit >>= 1) {
				if (!(cursor_plane(plane2 + 4 * r) & bit)) {
					px[c] = 0;
				}
				if (cursor_plane(plane1 + 4 * r) & bit) {
					px[c] = 7;
				}
			}
			row += pitch;
		}
	}
	SC_SET(cursor_saved_x, SC(cursor_x));
	SC_SET(cursor_saved_y, SC(cursor_y));
	return SC(cursor_x);
}

/* 0x401c40, 0x401c60: show or hide the game cursor (draw it, or take it
 * off, if that changes anything). */
void game_cursor_show(void)
{
	if (!SC(cursor_on)) {
		game_cursor_draw();
		SC_SET(cursor_on, 1);
		return;
	}
}

void game_cursor_hide(void)
{
	if (SC(cursor_on)) {
		game_cursor_restore();
		SC_SET(cursor_on, 0);
	}
}

/* 0x401c80: the host mouse pointer to (x, y), the screen shifted down when
 * letterbox is set. */
void game_cursor_setpos(uint32_t x, uint32_t y)
{
	if (SC(letterbox)) {
		y += SHIFT_LINES;
	}
	win_warp_cursor((int)x, (int)y);
}

/* 0x401190: lock the surface and return its pixels, after taking the cursor off the screen. On
 * the screen with letterbox set the pointer is 40 lines down. */
uint8_t *game_surface_lock(uint32_t surf)
{
	uint32_t screen = G.rt.surface[0];
	if (surf == screen) {
		if (SC(cursor_on)) {
			game_cursor_restore();
		}
	}
	uint8_t *p = surf_lock(surf, NULL);
	screen = G.rt.surface[0];
	if (surf == screen) {
		if (SC(letterbox)) {
			p += 8 * (G.rt.pitch[0] * 5); /* lea ecx, [ecx + 4*ecx] */
		}
	}
	return p;
}

/* 0x4012b0: unlock the surface, and draw the cursor again if it was the
 * screen. Returns EAX as the routine leaves it (0, or the screen's handle, or
 * the cursor's x): the last unlock's result is the S of op 19 sub-ops 3
 * and 4 and of the map's redraws. */
uint32_t game_surface_unlock(uint32_t surf)
{
	uint32_t screen = G.rt.surface[0];
	if (surf != screen) {
		return screen;
	}
	if (!SC(cursor_on)) {
		return 0;
	}
	return game_cursor_draw();
}

/*
 * Rectangles: copy, masked copy, swap, fill and XOR on the numbered surfaces. The copy
 * arguments are (dx, dy, sx, sy, w, h, src, dst): the source rectangle (sx, sy, w, h) of
 * surface src goes to (dx, dy) of surface dst (the native routine takes the two numbers in one
 * word, src << 8 | dst, of which it uses these 16 bits). On the screen with letterbox set every
 * rectangle and position on surface 0 is 40 lines lower. The cursor is taken off the screen
 * first and drawn again after, whatever surfaces are involved. Colour 8 is transparent in the
 * masked copy. The coordinates and sizes are the full 32-bit words the script handlers compute
 * (the upper halves carry over from their registers); the platform gets them as signed ints.
 */
#define TRANSPARENT 8u

static void cursor_off(void)
{
	if (SC(cursor_on)) {
		game_cursor_restore();
	}
}

static void cursor_on(void)
{
	if (SC(cursor_on)) {
		game_cursor_draw();
	}
}

/* BltFast(dst, x, y, src, rectangle, keyed), and its time (pace.h): a plain copy costs less than
 * one with the source's colour key */
static void bltfast(uint32_t dst, uint32_t x, uint32_t y, uint32_t src, uint32_t l, uint32_t t, uint32_t r,
                    uint32_t b, int keyed)
{
	int w = (int)(r - l), h = (int)(b - t);
	if (w > 0 && h > 0)
		ctl_work_us(PACE_BLIT_US(w, h, keyed && surf_has_colorkey(src)));
	surf_blt_fast(dst, (int)x, (int)y, src, (int)l, (int)t, (int)r, (int)b, keyed);
}

/* 0x401430 */
void game_blit_copy(uint32_t dx, uint32_t dy, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                    uint8_t src, uint8_t dst)
{
	uint32_t top = sy, bottom = sy + h;
	if (src == 0) {
		if (SC(letterbox)) {
			top += SHIFT_LINES;
			bottom += SHIFT_LINES;
		}
	}
	if (dst == 0) {
		if (SC(letterbox)) {
			dy += SHIFT_LINES;
		}
	}
	cursor_off();
	uint32_t dobj = surface_at(dst);
	bltfast(dobj, dx, dy, surface_at(src), sx, top, sx + w, bottom, 0);
	cursor_on();
}

/* 0x4014f0: same arguments; on the screen the surface's colour key (8)
 * does the masking, otherwise the rectangle is copied by hand. */
void game_blit_masked(uint32_t dx, uint32_t dy, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                      uint8_t src, uint8_t dst)
{
	if (dst == 0) {
		uint32_t top = sy, bottom = sy + h;
		int shift = 0;
		if (src == 0) {
			shift = SC(letterbox) != 0;
			if (shift) {
				top += SHIFT_LINES;
				bottom += SHIFT_LINES;
			}
		}
		if (src != 0 || shift) {
			if (SC(letterbox)) {
				dy += SHIFT_LINES;
			}
		}
		cursor_off();
		uint32_t screen = surface_at(0);
		bltfast(screen, dx, dy, surface_at(src), sx, top, sx + w, bottom, 1);
		cursor_on();
		return;
	}
	/* by hand: lock both, copy the rows, skipping colour 8 */
	uint32_t sobj = surface_at(src), dobj = surface_at(dst);
	uint8_t *sp = game_surface_lock(sobj);
	uint8_t *dp = game_surface_lock(dobj);
	uint32_t spitch = pitch_at(src), dpitch = pitch_at(dst);
	sp += sy * spitch + sx;
	dp += dpitch * dy + dx;
	if ((int32_t)h > 0) {
		for (uint32_t r = 0; r < h; r++) {
			if ((int32_t)w > 0) {
				for (uint32_t i = 0; i < w; i++) {
					uint32_t v = sp[i];
					if (v != TRANSPARENT) {
						dp[i] = (uint8_t)v;
					}
				}
			}
			sp += spitch;
			dp += dpitch;
		}
	}
	game_surface_unlock(dobj);
	game_surface_unlock(sobj);
}

/* 0x401670: swap the rectangle (sx, sy, w, h) of surface src with the one
 * at (dx, dy) of surface dst, through the scratch surface. */
void game_blit_swap(uint32_t dx, uint32_t dy, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                    uint8_t src, uint8_t dst)
{
	uint32_t top = sy, bottom = sy + h, dtop = dy, dbottom = dy + h;
	if (src == 0) {
		if (SC(letterbox)) {
			top += SHIFT_LINES;
			bottom += SHIFT_LINES;
		}
	}
	if (dst == 0) {
		if (SC(letterbox)) {
			dtop += SHIFT_LINES;
			dbottom += SHIFT_LINES;
		}
	}
	cursor_off();
	uint32_t scratch = SC(scratch_surface), sobj = surface_at(src), dobj = surface_at(dst);
	bltfast(scratch, 0, 0, sobj, sx, top, sx + w, bottom, 0);
	bltfast(sobj, sx, top, dobj, dx, dtop, dx + w, dbottom, 0);
	bltfast(dobj, dx, dtop, scratch, 0, 0, w, h, 0);
	cursor_on();
}

/* 0x4017b0 (x, y, w, h, colour << 4, surface): fill a rectangle with colour
 * (a Blt with DDBLT_COLORFILL). All words are the 32-bit values the caller
 * built (the text window's and the opcode's rectangles carry bits over into
 * their upper halves). */
void game_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour,
                    uint32_t surf)
{
	uint32_t top = y, bottom = y + h;
	if (surf == 0) {
		if (SC(letterbox)) {
			top += SHIFT_LINES;
			bottom += SHIFT_LINES;
		}
	}
	cursor_off();
	uint32_t obj = surface_at(surf);
	int iw = (int)w, ih = (int)(bottom - top);
	if (iw > 0 && ih > 0)
		ctl_work_us(PACE_BLIT_US(iw, ih, 0));
	surf_fill(obj, (int)x, (int)top, (int)(x + w), (int)bottom, (uint32_t)((int32_t)colour >> 4));
	cursor_on();
}

/* 0x401870 (x, y, w, h, colour << 4, surface): XOR the colour into the
 * pixels of a rectangle, four at a time (w is rounded up to a multiple of
 * 4). */
void game_xor_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour,
                   uint32_t surf)
{
	uint32_t c = colour >> 4, pat = c;
	pat = (pat << 8) + c;
	pat = (pat << 8) + c;
	pat = (pat << 8) + c;
	uint32_t obj = surface_at(surf);
	uint8_t *p = game_surface_lock(obj);
	uint32_t pitch = pitch_at(surf);
	p += pitch * y + x;
	if ((int32_t)h > 0) {
		for (uint32_t r = 0; r < h; r++) {
			if ((int32_t)w > 0) {
				for (uint32_t i = 0; (int32_t)i < (int32_t)w; i += 4) {
					uint32_t v;
					memcpy(&v, p + i, 4);
					v ^= pat;
					memcpy(p + i, &v, 4);
				}
			}
			p += pitch;
		}
	}
	game_surface_unlock(obj);
}

/*
 * Picture load 0x406e20. With `named` (the opcode had an
 * argument) the name is argument 0 of the opcode, read into STRING_BUF and
 * the file (a GP4 picture) loaded to PICTURE; otherwise the picture already
 * there is drawn again. The animation slots are reset first. The picture:
 * four u16 (x / 8, y, width / 8, height) and, unless @[9] bit 0 keeps the
 * palette, a palette block (0x40752c), then the 8-bit pixels from PICTURE +
 * 0x208; they go to the rectangle (x, y, width, height) of the surface @[6]
 * names, and the rectangle is kept at PICTURE_RECT for the masked copy op 04
 * 1b. Returns EAX as the last unlock leaves it (op 19 0 stores it as S).
 */
#define PICTURE 0x4662beu
#define PICTURE_NAME 0x445d26u
#define PICTURE_RECT 0x44538au /* u16 x / 8 ... y, w / 8, h (4 x u16, 0x44538a-0x445390) */

/* the picture buffer: segment 3's start in the image area */
static const uint8_t *picture_buf(void)
{
	return G.img + (PICTURE - IMG_BASE);
}

uint32_t game_picture_load(int named)
{
	game_anim_reset();
	if (named) {
		uint32_t src, last;
		vm_string_arg(PICTURE_NAME, &src, &last);
		game_file_load_at(PICTURE_NAME, PICTURE);
	}
	const uint8_t *pic = picture_buf();
	uint16_t hdr[4];
	memcpy(hdr, pic, sizeof hdr);
	SV_SET(pic_rect[0], hdr[0] >> 3);
	SV_SET(pic_rect[1], hdr[1]);
	SV_SET(pic_rect[2], hdr[2] >> 3);
	SV_SET(pic_rect[3], hdr[3]);
	if (!(SV(vflags) & 1)) {
		game_palette_copy_at(PICTURE + 8);
	}
	const uint8_t *src = pic + 0x208;
	uint32_t surf = SV(surface_select);
	uint8_t *dst = game_surface_lock(surface_at(surf));
	uint32_t pitch = pitch_at(surf);
	dst += SV(pic_rect[1]) * pitch + (SV(pic_rect[0]) << 3);
	uint32_t rows = SV(pic_rect[3]), dwords = SV(pic_rect[2]) << 1;
	while (rows) {
		memcpy(dst, src, 4 * dwords);
		src += 4 * dwords;
		dst += pitch;
		rows--;
	}
	uint32_t r = game_surface_unlock(surface_at(surf));
	return r;
}
