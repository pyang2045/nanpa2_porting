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
 * Instruction charges are the native routines' counts (their own
 * instructions, the `ret` included, not the `call`).
 */
#include "game/ctl.h"
#include "game/file.h"
#include "game/vm.h"
#include "x86/x86.h"
#include "game/state.h"

#define PAL_BASE_N 16u
#define PAL_PICTURE_N 240u
/* The tables (state.h): G.save.n.palette[16] 4:4:4 (0x0GRB, one nibble per channel), G.rt.palette_picture[240]
 * 5:5:5 (red in bits 10-14), G.rt.palette_saved[256] the same 16 + 240 colours saved (0x40756c),
 * G.sc.pal_entries PALETTEENTRY[256] (red, green, blue, flags), G.sc.pal_delta[256][3] target - displayed,
 * G.sc.pal_start the entries as the fade started. */
#define PAL_ENTRIES 0x4ac07cu  /* the guest address of pal_entries, passed to SetEntries */

/* 0x40756c / 0x407599: save the tables to PAL_SAVED, or put them back,
 * unless @[9] bit 4 is set. */
static void palette_copy_tables(int save)
{
	COST(2);
	if (SV(vflags) & 0x10) {
		COST(1);
		return;
	}
	if (save) {
		memcpy(G.rt.palette_saved, G.save.n.palette, sizeof G.save.n.palette);
		memcpy(G.rt.palette_saved + PAL_BASE_N, G.rt.palette_picture, sizeof G.rt.palette_picture);
	} else {
		memcpy(G.save.n.palette, G.rt.palette_saved, sizeof G.save.n.palette);
		memcpy(G.rt.palette_picture, G.rt.palette_saved + PAL_BASE_N, sizeof G.rt.palette_picture);
	}
	COST(14 + 8 + 0x78);
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
	COST(11 + PAL_BASE_N + PAL_PICTURE_N);
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
	uint32_t obj = SC(palette_obj);
	HLE(rd32(rd32(obj) + 0x18), obj, 0, 0, 0x100, PAL_ENTRIES);
	COST(3 + 14 * PAL_BASE_N + 2 + 16 * PAL_PICTURE_N + 8 + 2);
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
	COST(6 + 26 * PAL_BASE_N + 3 + 27 * PAL_PICTURE_N + 4 + 1 + 0x100 + 4);
	return last;
}

/* 0x401e10: whether a fade runs (the timer callback clears it) */
uint32_t game_fade_active(void)
{
	COST(2);
	return SC(fade_active);
}

/* 0x406f41: op 04 16 3 waits for the fade: the native code asks again (a
 * backward jump, which polls: the timer callback is what clears the flag)
 * until it is clear; then it puts the tables back. */
void game_palette_wait(void)
{
	for (;;) {
		uint32_t active = game_fade_active();
		COST(3);
		if (!active)
			break;
		POLL(0x406f41);
	}
	game_palette_restore();
	COST(1);
}

/*
 * Surfaces and the game cursor (stage 5B).
 *
 * The screen is surface 0 of SURFACES (DirectDraw objects, pitches in
 * PITCHES); the scratch surface 0x4ab058 and the rest are numbered
 * after it. Every routine that draws on or locks the screen first takes
 * the game's own cursor off it and puts it back afterwards: the cursor is
 * a 32x32 sprite (two bit planes per frame of CURSOR_BITS: where the
 * second plane is clear the pixel turns black, where the first is set it
 * turns white) drawn straight into the locked screen, with the pixels
 * under it kept in SAVED_BG so that they can be put back. When
 * SCREEN_SHIFT is set the picture is drawn 40 lines below the surface's
 * top, and every rectangle on surface 0 moves with it. Semantics:
 * re-notes.md "Surfaces and cursor".
 *
 * The routines are ordinary functions of their arguments; the charges are
 * the native routines' instruction counts. A surface lock hands out the
 * guest address of the pixels (the DirectDraw lock descriptor and
 * G.sc.lock_ptr hold it too); the drawing code reaches them through
 * x86_ptr for exactly the bytes a row touches.
 */
#define SURFACES 0x44629eu    /* u32[]: surface objects, [0] the screen */
#define PITCHES 0x4462aeu     /* u32[]: their pitches */
#define LOCK_DESC 0x4ac480u   /* DDSURFACEDESC of the last Lock */
/* G.sc.cursor_saved: 32 rows of 32 bytes; G.cursor_bits: the two planes (AND, XOR) of each frame, 0x100 apart, as
 * big-endian dwords per row. A frame number past the table reads on into the data that follows, as natively. */
static uint32_t cursor_plane(uint32_t off)
{
	uint32_t v;
	memcpy(&v, (uint8_t *)&G.cursor_bits + off, 4);
	return v;
}
#define IAT_SETCURSORPOS 0x41b12cu
#define SHIFT_LINES 0x28u

#define COM_BLT 0x14u
#define COM_BLTFAST 0x1cu
#define COM_LOCK 0x64u
#define COM_UNLOCK 0x80u

/* Scratch words on the emulated stack for the HLE boundary (the RECT and
 * DDBLTFX the DirectDraw methods read from guest memory, the string and
 * device context of the GDI calls) until R4 removes it. */
uint32_t game_frame_alloc(uint32_t bytes)
{
	cpu.r[ESP] -= bytes;
	return cpu.r[ESP];
}

void game_frame_free(uint32_t bytes)
{
	cpu.r[ESP] += bytes;
}

/* the surface object and pitch of surface i (the index comes from the
 * script unchecked: past 3 it reads on into the tables after) */
static uint32_t surface_at(uint32_t i)
{
	return rd32(SURFACES + 4 * i);
}

static uint32_t pitch_at(uint32_t i)
{
	return rd32(PITCHES + 4 * i);
}

/* the DirectDraw method at a vtable offset of the object */
#define COM(obj, off, ...) HLE(rd32(rd32(obj) + (off)), (obj), __VA_ARGS__)

static void com_lock(uint32_t surf)
{
	COM(surf, COM_LOCK, 0, LOCK_DESC, 1, 0);
}

static void com_unlock(uint32_t surf)
{
	COM(surf, COM_UNLOCK, 0);
}

/* 0x4011f0: put the pixels under the cursor back (the width and height
 * clip at the screen's edge). */
void game_cursor_restore(void)
{
	uint32_t x = SC(cursor_saved_x), y = SC(cursor_saved_y), w, h;
	COST(7);
	if ((int32_t)x > 0x260) {
		w = 0x280 - x;
		COST(3);
	} else {
		w = 0x20;
		COST(1);
	}
	COST(4);
	if ((int32_t)y > 0x1c0) {
		h = 0x1e0 - y;
		COST(3);
	} else {
		h = 0x20;
		COST(1);
	}
	uint32_t screen = G.rt.surface[0];
	com_lock(screen);
	COST(8);
	uint32_t pitch = G.rt.pitch[0], row = SC(lock_ptr) + pitch * y + x;
	COST(8);
	if ((int32_t)h > 0) {
		COST(5);
		for (uint32_t r = 0; r < h; r++) {
			memcpy(x86_ptr(row, w), G.sc.cursor_saved + 32 * r, w);
			row += pitch;
			COST(14 + (r ? 1 : 0) + w / 4 + w % 4);
		}
	}
	COST(5);
	com_unlock(screen);
	COST(5);
}

/* 0x4012e0: save the pixels under the cursor and draw it at its place.
 * Returns EAX as the routine leaves it, the cursor's x. */
uint32_t game_cursor_draw(void)
{
	uint32_t x = SC(cursor_x), y = SC(cursor_y), w, h;
	COST(8);
	if ((int32_t)x > 0x260) {
		w = 0x280 - x;
		COST(3);
	} else {
		w = 0x20;
		COST(1);
	}
	COST(4);
	if ((int32_t)y > 0x1c0) {
		h = 0x1e0 - y;
		COST(3);
	} else {
		h = 0x20;
		COST(1);
	}
	uint32_t frame = SC(cursor_frame);
	uint32_t plane1 = frame << 8, plane2 = 0x80 + (frame << 8);
	uint32_t screen = G.rt.surface[0];
	com_lock(screen);
	COST(15);
	uint32_t pitch = G.rt.pitch[0], top = SC(lock_ptr) + pitch * y + x;
	COST(9);
	if ((int32_t)h > 0) {
		COST(3);
		uint32_t row = top;
		for (uint32_t r = 0; r < h; r++) {
			memcpy(G.sc.cursor_saved + 32 * r, x86_ptr(row, w), w);
			row += pitch;
			COST(16 + (r ? 1 : 0) + w / 4 + w % 4);
		}
		COST(6);
		row = top;
		for (uint32_t r = 0; r < h; r++) {
			COST(4);
			uint32_t bit = 0x80000000u;
			uint8_t *px = (int32_t)w > 0 ? x86_ptr(row, w) : NULL;
			for (uint32_t c = 0; (int32_t)w > 0 && c < w; c++, bit >>= 1) {
				COST(8);
				if (!(cursor_plane(plane2 + 4 * r) & bit)) {
					px[c] = 0;
					COST(1);
				}
				if (cursor_plane(plane1 + 4 * r) & bit) {
					px[c] = 7;
					COST(1);
				}
			}
			row += pitch;
			COST(6);
		}
	}
	COST(5);
	com_unlock(screen);
	SC_SET(cursor_saved_x, SC(cursor_x));
	SC_SET(cursor_saved_y, SC(cursor_y));
	COST(11);
	return SC(cursor_x);
}

/* 0x401c40, 0x401c60: show or hide the game cursor (draw it, or take it
 * off, if that changes anything). */
void game_cursor_show(void)
{
	COST(3);
	if (!SC(cursor_on)) {
		COST(1);
		game_cursor_draw();
		SC_SET(cursor_on, 1);
		COST(2);
		return;
	}
	COST(1);
}

void game_cursor_hide(void)
{
	COST(3);
	if (SC(cursor_on)) {
		COST(1);
		game_cursor_restore();
		SC_SET(cursor_on, 0);
		COST(1);
	}
	COST(1);
}

/* 0x401c80: the host mouse pointer to (x, y), the screen shifted down when
 * SCREEN_SHIFT is set. */
void game_cursor_setpos(uint32_t x, uint32_t y)
{
	COST(3);
	if (SC(letterbox)) {
		y += SHIFT_LINES;
		COST(1);
	}
	COST(5);
	HLE(rd32(IAT_SETCURSORPOS), x, y);
	COST(1);
}

/* 0x401190: lock the surface (the object's address) and return the guest
 * address of its pixels, after taking the cursor off the screen. On the
 * screen with SCREEN_SHIFT set the pointer is 40 lines down. */
uint32_t game_surface_lock(uint32_t surf)
{
	uint32_t screen = G.rt.surface[0];
	COST(5);
	if (surf == screen) {
		COST(3);
		if (SC(cursor_on)) {
			COST(1);
			game_cursor_restore();
		}
	}
	com_lock(surf);
	COST(7);
	screen = G.rt.surface[0];
	uint32_t p = SC(lock_ptr);
	COST(5);
	if (surf == screen) {
		COST(3);
		if (SC(letterbox)) {
			p += 8 * (G.rt.pitch[0] * 5); /* lea ecx, [ecx + 4*ecx] */
			COST(3);
		}
	}
	COST(1);
	return p;
}

/* 0x4012b0: unlock the surface, and draw the cursor again if it was the
 * screen. Returns EAX as the routine leaves it (0, or the screen object, or
 * the cursor's x): the last unlock's result is the S of op 19 sub-ops 3
 * and 4 and of the map's redraws. */
uint32_t game_surface_unlock(uint32_t surf)
{
	COST(6);
	com_unlock(surf);
	uint32_t screen = G.rt.surface[0];
	COST(4);
	if (surf != screen) {
		COST(1);
		return screen;
	}
	COST(3);
	if (!SC(cursor_on)) {
		COST(1);
		return 0;
	}
	COST(1);
	return game_cursor_draw();
}

/* The register-level entries of the two routines, for the native-style
 * callers that remain (map_draw.c's game_ncall); cdecl, the argument at
 * ESP, the result in EAX and the registers the native routines left
 * (ECX after the lock). */
void game_lock_regs(void)    /* 0x401190 */
{
	uint32_t surf = rd32(cpu.r[ESP]);
	cpu.r[EAX] = game_surface_lock(surf);
	uint32_t screen = G.rt.surface[0];
	cpu.r[ECX] = screen;
	if (surf == screen) {
		cpu.r[ECX] = SC(letterbox);
		if (cpu.r[ECX])
			cpu.r[ECX] = G.rt.pitch[0] * 5;
	}
}

void game_unlock_regs(void)  /* 0x4012b0 */
{
	cpu.r[EAX] = game_surface_unlock(rd32(cpu.r[ESP]));
}

/*
 * Rectangles (stage 5B): copy, masked copy, swap, fill and XOR on the
 * numbered surfaces. The copy arguments are (dx, dy, sx, sy, w, h, src,
 * dst): the source rectangle (sx, sy, w, h) of surface src goes to (dx,
 * dy) of surface dst (the native routine takes the two numbers in one word,
 * src << 8 | dst, of which it uses these 16 bits). On the screen with
 * SCREEN_SHIFT set every rectangle and position on surface 0 is 40 lines
 * lower. The cursor is taken off the screen first and drawn again after,
 * whatever surfaces are involved. Colour 8 is transparent in the masked
 * copy. The coordinates and sizes are the full 32-bit words the script
 * handlers compute (the upper halves carry over from their registers); the
 * rectangle given to DirectDraw holds them all.
 */
#define BLITFAST_WAIT 0x10u
#define BLITFAST_KEYED 0x11u  /* wait, source colour key */
#define BLT_COLORFILL_WAIT 0x1000400u
#define TRANSPARENT 8u

/* mov eax, [cursor]; test; je; [call 0x4011f0] (n further instructions
 * between them for the routine's own moves) */
static void cursor_off(int n)
{
	COST(3 + n);
	if (SC(cursor_on)) {
		COST(1);
		game_cursor_restore();
	}
}

static void cursor_on(int n)
{
	COST(3 + n);
	if (SC(cursor_on)) {
		COST(1);
		game_cursor_draw();
	}
}

static void put_rect(uint32_t at, uint32_t l, uint32_t t, uint32_t r, uint32_t b)
{
	wr32(at, l);
	wr32(at + 4, t);
	wr32(at + 8, r);
	wr32(at + 12, b);
}

/* BltFast(this = dst, x, y, src, &rect, flags) */
static void bltfast(uint32_t dst, uint32_t x, uint32_t y, uint32_t src, uint32_t rect,
                    uint32_t flags)
{
	COM(dst, COM_BLTFAST, x, y, src, rect, flags);
}

/* 0x401430 */
void game_blit_copy(uint32_t dx, uint32_t dy, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                    uint8_t src, uint8_t dst)
{
	uint32_t top = sy, bottom = sy + h;
	COST(23);
	if (src == 0) {
		COST(2);
		if (SC(letterbox)) {
			top += SHIFT_LINES;
			bottom += SHIFT_LINES;
			COST(5);
		}
	}
	COST(2);
	if (dst == 0) {
		COST(2);
		if (SC(letterbox)) {
			dy += SHIFT_LINES;
			COST(1);
		}
	}
	uint32_t rect = game_frame_alloc(0x10);
	put_rect(rect, sx, top, sx + w, bottom);
	cursor_off(0);
	uint32_t dobj = surface_at(dst);
	COST(13);
	bltfast(dobj, dx, dy, surface_at(src), rect, BLITFAST_WAIT);
	game_frame_free(0x10);
	cursor_on(2);
	COST(2);
}

/* 0x4014f0: same arguments; on the screen the surface's colour key (8)
 * does the masking, otherwise the rectangle is copied by hand. */
void game_blit_masked(uint32_t dx, uint32_t dy, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                      uint8_t src, uint8_t dst)
{
	COST(9);
	if (dst == 0) {
		uint32_t top = sy, bottom = sy + h;
		int shift = 0;
		COST(14);
		if (src == 0) {
			COST(2);
			shift = SC(letterbox) != 0;
			if (shift) {
				top += SHIFT_LINES;
				bottom += SHIFT_LINES;
				COST(5);
			}
		}
		if (src != 0 || shift) {
			COST(2);
			if (SC(letterbox)) {
				dy += SHIFT_LINES;
				COST(1);
			}
		}
		uint32_t rect = game_frame_alloc(0x10);
		put_rect(rect, sx, top, sx + w, bottom);
		cursor_off(0);
		uint32_t screen = surface_at(0);
		COST(13);
		bltfast(screen, dx, dy, surface_at(src), rect, BLITFAST_KEYED);
		game_frame_free(0x10);
		cursor_on(0);
		COST(4);
		return;
	}
	/* by hand: lock both, copy the rows, skipping colour 8 */
	uint32_t sobj = surface_at(src), dobj = surface_at(dst);
	COST(7);
	uint32_t sp = game_surface_lock(sobj);
	COST(6);
	uint32_t dp = game_surface_lock(dobj);
	uint32_t spitch = pitch_at(src), dpitch = pitch_at(dst);
	sp += sy * spitch + sx;
	dp += dpitch * dy + dx;
	COST(17);
	if ((int32_t)h > 0) {
		COST(1);
		for (uint32_t r = 0; r < h; r++) {
			COST(3);
			if ((int32_t)w > 0) {
				COST(4);
				const uint8_t *s = x86_ptr(sp, w);
				uint8_t *d = x86_ptr(dp, w);
				for (uint32_t i = 0; i < w; i++) {
					uint32_t v = s[i];
					COST(6);
					if (v != TRANSPARENT) {
						d[i] = (uint8_t)v;
						COST(1);
					}
				}
			}
			sp += spitch;
			dp += dpitch;
			COST(8);
			if (r + 1 < h)
				POLL(0x4015b1);
		}
	}
	COST(4);
	game_surface_unlock(dobj);
	COST(4);
	game_surface_unlock(sobj);
	COST(1 + 4 + 2);
}

/* 0x401670: swap the rectangle (sx, sy, w, h) of surface src with the one
 * at (dx, dy) of surface dst, through the scratch surface. */
void game_blit_swap(uint32_t dx, uint32_t dy, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                    uint8_t src, uint8_t dst)
{
	uint32_t top = sy, bottom = sy + h, dtop = dy, dbottom = dy + h;
	COST(38);
	if (src == 0) {
		COST(2);
		if (SC(letterbox)) {
			top += SHIFT_LINES;
			bottom += SHIFT_LINES;
			COST(5);
		}
	}
	COST(2);
	if (dst == 0) {
		COST(2);
		if (SC(letterbox)) {
			dtop += SHIFT_LINES;
			dbottom += SHIFT_LINES;
			COST(5);
		}
	}
	uint32_t r1 = game_frame_alloc(0x30), r2 = r1 + 0x10, r3 = r1 + 0x20;
	put_rect(r1, sx, top, sx + w, bottom);
	put_rect(r2, dx, dtop, dx + w, dbottom);
	put_rect(r3, 0, 0, w, h);
	cursor_off(-1); /* cmp [cursor], edi; je */
	uint32_t scratch = SC(scratch_surface), sobj = surface_at(src), dobj = surface_at(dst);
	COST(11);
	bltfast(scratch, 0, 0, sobj, r1, BLITFAST_WAIT);
	COST(13);
	bltfast(sobj, sx, top, dobj, r2, BLITFAST_WAIT);
	COST(13);
	bltfast(dobj, dx, dtop, scratch, r3, BLITFAST_WAIT);
	game_frame_free(0x30);
	cursor_on(4);
	COST(2);
}

/* 0x4017b0 (x, y, w, h, colour << 4, surface): fill a rectangle with colour
 * (a Blt with DDBLT_COLORFILL). All words are the 32-bit values the caller
 * built (the text window's and the opcode's rectangles carry bits over into
 * their upper halves). */
void game_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour,
                    uint32_t surf)
{
	uint32_t top = y, bottom = y + h;
	COST(15);
	if (surf == 0) {
		COST(3);
		if (SC(letterbox)) {
			top += SHIFT_LINES;
			bottom += SHIFT_LINES;
			COST(5);
		}
	}
	uint32_t rect = game_frame_alloc(0x74), fx = rect + 0x10; /* RECT, DDBLTFX */
	put_rect(rect, x, top, x + w, bottom);
	for (uint32_t i = 0; i < 0x19; i++)
		wr32(fx + 4 * i, 0);
	wr32(fx, 0x64);
	wr32(fx + 0x50, (uint32_t)((int32_t)colour >> 4));
	COST(5 + 0x19 + 4);
	cursor_off(1);
	uint32_t obj = surface_at(surf);
	COST(11);
	COM(obj, COM_BLT, rect, 0, 0, BLT_COLORFILL_WAIT, fx);
	game_frame_free(0x74);
	cursor_on(1);
	COST(2);
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
	COST(17);
	uint32_t p = game_surface_lock(obj);
	uint32_t pitch = pitch_at(surf);
	p += pitch * y + x;
	COST(10);
	if ((int32_t)h > 0) {
		COST(4);
		for (uint32_t r = 0; r < h; r++) {
			COST((r ? 1 : 0) + 2);
			if ((int32_t)w > 0) {
				uint8_t *px = x86_ptr(p, (w + 3) & ~3u);
				for (uint32_t i = 0; (int32_t)i < (int32_t)w; i += 4) {
					uint32_t v;
					memcpy(&v, px + i, 4);
					v ^= pat;
					memcpy(px + i, &v, 4);
					COST(4);
				}
			}
			p += pitch;
			COST(4);
			if (r + 1 < h)
				POLL(0x4018e0);
		}
		COST(1);
	}
	COST(3);
	game_surface_unlock(obj);
	COST(5);
}

/*
 * Picture load 0x406e20 (stage 5B). With `named` (the opcode had an
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
	COST(2);
	game_anim_reset();
	COST(3);
	if (named) {
		cpu.r[EDI] = PICTURE_NAME;
		game_string_arg_regs();
		COST(6);
		game_file_load_at(PICTURE_NAME, PICTURE);
	}
	const uint8_t *pic = picture_buf();
	uint16_t hdr[4];
	memcpy(hdr, pic, sizeof hdr);
	SV_SET(pic_rect[0], hdr[0] >> 3);
	SV_SET(pic_rect[1], hdr[1]);
	SV_SET(pic_rect[2], hdr[2] >> 3);
	SV_SET(pic_rect[3], hdr[3]);
	COST(15);
	if (!(SV(vflags) & 1)) {
		COST(2);
		game_palette_copy_at(PICTURE + 8);
	}
	const uint8_t *src = pic + 0x208;
	uint32_t surf = SV(surface_select);
	uint32_t dst = game_surface_lock(surface_at(surf));
	uint32_t pitch = pitch_at(surf);
	dst += SV(pic_rect[1]) * pitch + (SV(pic_rect[0]) << 3);
	uint32_t rows = SV(pic_rect[3]), dwords = SV(pic_rect[2]) << 1;
	COST(18);
	while (rows) {
		memcpy(x86_ptr(dst, 4 * dwords), src, 4 * dwords);
		src += 4 * dwords;
		dst += pitch;
		rows--;
		COST(7 + dwords);
		if (rows)
			POLL(0x406ee1);
	}
	COST(4);
	uint32_t r = game_surface_unlock(surface_at(surf));
	COST(4);
	return r;
}
