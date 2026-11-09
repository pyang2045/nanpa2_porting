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
#include "game/file.h"
#include "game/vm.h"
#include "x86/x86.h"
#include "game/state.h"

#define PAL_BASE 0x44530cu     /* u16[16]: 0x0GRB, one nibble per channel */
#define PAL_PICTURE 0x445eaeu  /* u16[240]: 5:5:5, red in bits 10-14 */
#define PAL_SAVED 0x44608eu    /* the same 16 + 240 colours, saved (0x40756c) */
#define PAL_ENTRIES 0x4ac07cu  /* PALETTEENTRY[256]: red, green, blue, flags */
#define PAL_DELTAS 0x4ab060u   /* i32[256][3]: target - displayed, per channel */
#define PAL_START 0x4aac58u    /* the entries as the fade started */
#define PAL_BASE_N 16u
#define PAL_PICTURE_N 240u

/* 0x40756c / 0x407599: save the tables to PAL_SAVED, or put them back,
 * unless @[9] bit 4 is set. Registers are kept. */
static void palette_copy_tables(int save)
{
	X86_COUNT(2);
	if (SV(vflags) & 0x10) {
		X86_COUNT(1);
		return;
	}
	for (uint32_t i = 0; i < PAL_BASE_N / 2; i++) {
		if (save)
			wr32(PAL_SAVED + 4 * i, rd32(PAL_BASE + 4 * i));
		else
			wr32(PAL_BASE + 4 * i, rd32(PAL_SAVED + 4 * i));
	}
	for (uint32_t i = 0; i < PAL_PICTURE_N / 2; i++) {
		uint32_t o = PAL_BASE_N * 2 + 4 * i;
		if (save)
			wr32(PAL_SAVED + o, rd32(PAL_PICTURE + 4 * i));
		else
			wr32(PAL_PICTURE + 4 * i, rd32(PAL_SAVED + o));
	}
	X86_COUNT(14 + 8 + 0x78);
}

void game_palette_save(void)    /* 0x40756c */
{
	palette_copy_tables(1);
}

void game_palette_restore(void) /* 0x407599 */
{
	palette_copy_tables(0);
}

/* 0x40754d: all 16 + 240 colours = v. */
void game_palette_fill(uint32_t v)
{
	for (uint32_t i = 0; i < PAL_BASE_N; i++)
		wr16(PAL_BASE + 2 * i, v);
	for (uint32_t i = 0; i < PAL_PICTURE_N; i++)
		wr16(PAL_PICTURE + 2 * i, v);
	X86_COUNT(11 + PAL_BASE_N + PAL_PICTURE_N);
}

/* the stdcall method of a COM object at a vtable offset: the arguments
 * (without the object) are on the stack */
static void com_call(uint32_t obj, uint32_t off, uint32_t nargs)
{
	game_ncall(rd32(rd32(obj) + off), nargs);
}

/* 0x401ca0: build the 256 entries from the tables and give them to the
 * DirectDraw palette (SetEntries(0, 0, 256, entries)). */
void game_palette_upload(void)
{
	for (uint32_t i = 0; i < PAL_BASE_N; i++) {
		uint32_t w = rd16(PAL_BASE + 2 * i), e = PAL_ENTRIES + 4 * i;
		wr8(e, w & 0xf0);
		wr8(e + 1, (w >> 4) & 0xf0);
		wr8(e + 2, (w & 0x0f) << 4);
	}
	for (uint32_t i = 0; i < PAL_PICTURE_N; i++) {
		uint32_t w = rd16(PAL_PICTURE + 2 * i), e = PAL_ENTRIES + 4 * (PAL_BASE_N + i);
		wr8(e, (w >> 7) & 0xf8);
		wr8(e + 1, (w >> 2) & 0xf8);
		wr8(e + 2, (w & 0x1f) << 3);
	}
	uint32_t obj = SC(palette_obj);
	push32(PAL_ENTRIES);
	push32(0x100);
	push32(0);
	push32(0);
	push32(obj);
	com_call(obj, 0x18, 5);
	cpu.r[ECX] = rd32(obj); /* the vtable the call went through */
	{ /* EDX: the last blue's loop leaves the shifted word, ESI's upper half is 0x4a */
		uint32_t w = rd16(PAL_PICTURE + 2 * (PAL_PICTURE_N - 1)), v = (0x004a0000u | w) >> 2;
		cpu.r[EDX] = (v & ~0xffu) | ((w << 3) & 0xff);
	}
	X86_COUNT(3 + 14 * PAL_BASE_N + 2 + 16 * PAL_PICTURE_N + 8 + 2);
}

/* 0x401d30: start a fade from the displayed entries to the tables. Out:
 * EAX the last blue distance, ECX 0 (the registers a script op 04 16 3
 * leaves for the VM: ECX's upper half is read by ops 04 1f). */
void game_fade_start(void)
{
	for (uint32_t i = 0; i < PAL_BASE_N; i++) {
		uint32_t w = rd16(PAL_BASE + 2 * i), e = PAL_ENTRIES + 4 * i, d = PAL_DELTAS + 12 * i;
		wr32(d, (w & 0xf0) - rd8(e));
		wr32(d + 4, ((w >> 4) & 0xf0) - rd8(e + 1));
		wr32(d + 8, ((w & 0x0f) << 4) - rd8(e + 2));
	}
	for (uint32_t i = 0; i < PAL_PICTURE_N; i++) {
		uint32_t w = rd16(PAL_PICTURE + 2 * i), e = PAL_ENTRIES + 4 * (PAL_BASE_N + i);
		uint32_t d = PAL_DELTAS + 12 * (PAL_BASE_N + i);
		wr32(d, ((w >> 7) & 0xf8) - rd8(e));
		wr32(d + 4, ((w >> 2) & 0xf8) - rd8(e + 1));
		wr32(d + 8, ((w & 0x1f) << 3) - rd8(e + 2));
		cpu.r[EAX] = ((w & 0x1f) << 3) - rd8(e + 2);
	}
	for (uint32_t i = 0; i < 0x100; i++)
		wr32(PAL_START + 4 * i, rd32(PAL_ENTRIES + 4 * i));
	SC_SET(fade_active, 1);
	cpu.r[ECX] = 0; /* rep movsd */
	cpu.r[EDX] = rd8(PAL_ENTRIES + 4 * 255 + 2);
	X86_COUNT(6 + 26 * PAL_BASE_N + 3 + 27 * PAL_PICTURE_N + 4 + 1 + 0x100 + 4);
}

/* 0x401e10 */
uint32_t game_fade_active(void)
{
	X86_COUNT(2);
	cpu.r[EAX] = SC(fade_active);
	return cpu.r[EAX];
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
 * The routines are cdecl with their arguments on the emulated stack at
 * ESP (as hle_call leaves them): a caller in C pushes them and calls the
 * `_regs` entry; the result is in EAX. Registers are left as the native
 * routines leave them (ECX's upper half is read by op 04 1f), and the
 * charges are their instruction counts.
 */
#define SURFACES 0x44629eu    /* u32[]: surface objects, [0] the screen */
#define PITCHES 0x4462aeu     /* u32[]: their pitches */
#define LOCK_DESC 0x4ac480u   /* DDSURFACEDESC of the last Lock */
#define CURSOR_SAVED 0x4abc60u /* 32 rows of 32 bytes */
#define CURSOR_BITS 0x4be900u
#define IAT_SETCURSORPOS 0x41b12cu
#define SHIFT_LINES 0x28u

#define COM_BLT 0x14u
#define COM_BLTFAST 0x1cu
#define COM_LOCK 0x64u
#define COM_UNLOCK 0x80u

static uint32_t arg(uint32_t i)
{
	return rd32(cpu.r[ESP] + 4 * i);
}

/* a COM call with its arguments pushed (this last) */
static void com(uint32_t obj, uint32_t off, uint32_t nargs)
{
	game_ncall(rd32(rd32(obj) + off), nargs);
}

static void com_lock(uint32_t surf)
{
	push32(0);
	push32(1);
	push32(LOCK_DESC);
	push32(0);
	push32(surf);
	com(surf, COM_LOCK, 5);
}

static void com_unlock(uint32_t surf)
{
	push32(0);
	push32(surf);
	com(surf, COM_UNLOCK, 2);
}

/* 0x4011f0: put the pixels under the cursor back (the width and height
 * clip at the screen's edge). Out: EAX 0, EDX the screen's vtable. */
void game_cursor_restore(void)
{
	uint32_t x = SC(cursor_saved_x), y = SC(cursor_saved_y), w, h;
	X86_COUNT(7);
	if ((int32_t)x > 0x260) {
		w = 0x280 - x;
		X86_COUNT(3);
	} else {
		w = 0x20;
		X86_COUNT(1);
	}
	X86_COUNT(4);
	if ((int32_t)y > 0x1c0) {
		h = 0x1e0 - y;
		X86_COUNT(3);
	} else {
		h = 0x20;
		X86_COUNT(1);
	}
	uint32_t screen = rd32(SURFACES);
	com_lock(screen);
	X86_COUNT(8);
	uint32_t pitch = rd32(PITCHES), row = SC(lock_ptr) + pitch * y + x;
	X86_COUNT(8);
	if ((int32_t)h > 0) {
		X86_COUNT(5);
		for (uint32_t r = 0; r < h; r++) {
			for (uint32_t i = 0; i < w; i++)
				wr8(row + i, rd8(CURSOR_SAVED + 32 * r + i));
			row += pitch;
			X86_COUNT(14 + (r ? 1 : 0) + w / 4 + w % 4);
		}
	}
	X86_COUNT(5);
	com_unlock(screen);
	cpu.r[EDX] = rd32(screen);
	cpu.r[ECX] = w; /* the width was stored over ECX's saved slot ([esp+0xc]) */
	X86_COUNT(5);
}

/* 0x4012e0: save the pixels under the cursor and draw it at its place. */
void game_cursor_draw(void)
{
	uint32_t x = SC(cursor_x), y = SC(cursor_y), w, h;
	X86_COUNT(8);
	if ((int32_t)x > 0x260) {
		w = 0x280 - x;
		X86_COUNT(3);
	} else {
		w = 0x20;
		X86_COUNT(1);
	}
	X86_COUNT(4);
	if ((int32_t)y > 0x1c0) {
		h = 0x1e0 - y;
		X86_COUNT(3);
	} else {
		h = 0x20;
		X86_COUNT(1);
	}
	uint32_t frame = SC(cursor_frame);
	uint32_t plane1 = CURSOR_BITS + (frame << 8), plane2 = CURSOR_BITS + 0x80 + (frame << 8);
	uint32_t screen = rd32(SURFACES);
	com_lock(screen);
	X86_COUNT(15);
	uint32_t pitch = rd32(PITCHES), top = SC(lock_ptr) + pitch * y + x;
	X86_COUNT(9);
	if ((int32_t)h > 0) {
		X86_COUNT(3);
		uint32_t row = top;
		for (uint32_t r = 0; r < h; r++) {
			for (uint32_t i = 0; i < w; i++)
				wr8(CURSOR_SAVED + 32 * r + i, rd8(row + i));
			row += pitch;
			X86_COUNT(16 + (r ? 1 : 0) + w / 4 + w % 4);
		}
		X86_COUNT(6);
		row = top;
		for (uint32_t r = 0; r < h; r++) {
			X86_COUNT(4);
			uint32_t bit = 0x80000000u;
			for (uint32_t c = 0; (int32_t)w > 0 && c < w; c++, bit >>= 1) {
				X86_COUNT(8);
				if (!(rd32(plane2 + 4 * r) & bit)) {
					wr8(row + c, 0);
					X86_COUNT(1);
				}
				if (rd32(plane1 + 4 * r) & bit) {
					wr8(row + c, 7);
					X86_COUNT(1);
				}
			}
			row += pitch;
			X86_COUNT(6);
		}
	}
	X86_COUNT(5);
	com_unlock(screen);
	cpu.r[EAX] = SC(cursor_x);
	cpu.r[ECX] = SC(cursor_y);
	SC_SET(cursor_saved_x, cpu.r[EAX]);
	SC_SET(cursor_saved_y, cpu.r[ECX]);
	cpu.r[EDX] = rd32(screen);
	X86_COUNT(11);
}

/* 0x401c40, 0x401c60: show or hide the game cursor (draw it, or take it
 * off, if that changes anything). */
void game_cursor_show(void)
{
	cpu.r[EAX] = SC(cursor_on);
	X86_COUNT(3);
	if (!cpu.r[EAX]) {
		X86_COUNT(1);
		game_cursor_draw();
		SC_SET(cursor_on, 1);
		X86_COUNT(2);
		return;
	}
	X86_COUNT(1);
}

void game_cursor_hide(void)
{
	cpu.r[EAX] = SC(cursor_on);
	X86_COUNT(3);
	if (cpu.r[EAX]) {
		X86_COUNT(1);
		game_cursor_restore();
		SC_SET(cursor_on, 0);
		X86_COUNT(1);
	}
	X86_COUNT(1);
}

/* 0x401c80 (cdecl x, y): the host mouse pointer to (x, y), the screen
 * shifted down when SCREEN_SHIFT is set. */
void game_cursor_setpos(void)
{
	uint32_t sp = cpu.r[ESP];
	X86_COUNT(3);
	if (SC(letterbox)) {
		wr32(sp + 4, rd32(sp + 4) + SHIFT_LINES);
		X86_COUNT(1);
	}
	cpu.r[EAX] = rd32(sp + 4);
	cpu.r[ECX] = rd32(sp);
	push32(cpu.r[EAX]);
	push32(cpu.r[ECX]);
	X86_COUNT(5);
	game_ncall(rd32(IAT_SETCURSORPOS), 2);
	X86_COUNT(1);
}

/* 0x401190 (cdecl surface): lock it and return the pixel pointer in EAX,
 * after taking the cursor off the screen. On the screen with SCREEN_SHIFT
 * set the pointer is 40 lines down. Out: ECX = the screen object. */
uint32_t game_surface_lock(uint32_t surf)
{
	uint32_t screen = rd32(SURFACES);
	X86_COUNT(5);
	if (surf == screen) {
		cpu.r[EAX] = SC(cursor_on);
		X86_COUNT(3);
		if (cpu.r[EAX]) {
			X86_COUNT(1);
			game_cursor_restore();
		}
	}
	com_lock(surf);
	X86_COUNT(7);
	screen = rd32(SURFACES);
	uint32_t p = SC(lock_ptr);
	cpu.r[ECX] = screen;
	X86_COUNT(5);
	if (surf == screen) {
		cpu.r[ECX] = SC(letterbox);
		X86_COUNT(3);
		if (cpu.r[ECX]) {
			cpu.r[ECX] = rd32(PITCHES) * 5; /* lea ecx, [ecx + 4*ecx] */
			p += 8 * cpu.r[ECX];
			X86_COUNT(3);
		}
	}
	X86_COUNT(1);
	cpu.r[EAX] = p;
	return p;
}

/* 0x4012b0 (cdecl surface): unlock it, and draw the cursor again if it
 * was the screen. Out: EAX 0 or the screen object, as natively. */
void game_surface_unlock(uint32_t surf)
{
	X86_COUNT(6);
	com_unlock(surf);
	uint32_t screen = rd32(SURFACES);
	cpu.r[EAX] = screen;
	X86_COUNT(4);
	if (surf != screen) {
		X86_COUNT(1);
		return;
	}
	cpu.r[EAX] = SC(cursor_on);
	X86_COUNT(3);
	if (!cpu.r[EAX]) {
		X86_COUNT(1);
		return;
	}
	X86_COUNT(1);
	game_cursor_draw();
}

void game_lock_regs(void)    /* 0x401190 */
{
	game_surface_lock(arg(0));
}

void game_unlock_regs(void)  /* 0x4012b0 */
{
	game_surface_unlock(arg(0));
}



/*
 * Rectangles (stage 5B): copy, masked copy, swap, fill and XOR on the
 * numbered surfaces. The copy arguments are cdecl (dx, dy, sx, sy, w, h,
 * surfaces): the source rectangle (sx, sy, w, h) of surface surfaces >> 8
 * goes to (dx, dy) of surface surfaces & 255. On the screen with
 * SCREEN_SHIFT set every rectangle and position on surface 0 is 40 lines
 * lower (a destination y on the screen is raised in the argument slot
 * itself). The cursor is taken off the screen first and drawn again
 * after, whatever surfaces are involved. Colour 8 is transparent in the
 * masked copy.
 */
#define BLITFAST_WAIT 0x10u
#define BLITFAST_KEYED 0x11u  /* wait, source colour key */
#define BLT_COLORFILL_WAIT 0x1000400u
#define TRANSPARENT 8u

static uint32_t surface_at(uint32_t i)
{
	return rd32(SURFACES + 4 * i);
}

/* mov eax, [cursor]; test; je; [call 0x4011f0] (n further instructions
 * between them for the routine's own moves) */
static void cursor_off(int n)
{
	cpu.r[EAX] = SC(cursor_on);
	X86_COUNT(3 + n);
	if (cpu.r[EAX]) {
		X86_COUNT(1);
		game_cursor_restore();
	}
}

static void cursor_on(int n)
{
	cpu.r[EAX] = SC(cursor_on);
	X86_COUNT(3 + n);
	if (cpu.r[EAX]) {
		X86_COUNT(1);
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
	push32(flags);
	push32(rect);
	push32(src);
	push32(y);
	push32(x);
	push32(dst);
	com(dst, COM_BLTFAST, 6);
}

/* 0x401430 */
void game_blit_copy(void)
{
	uint32_t sx = arg(2), sy = arg(3), w = arg(4), h = arg(5);
	uint32_t src = (arg(6) >> 8) & 0xff, dst = arg(6) & 0xff;
	uint32_t top = sy, bottom = sy + h;
	uint32_t slot_y = cpu.r[ESP] + 4;
	X86_COUNT(23);
	if (src == 0) {
		X86_COUNT(2);
		if (SC(letterbox)) {
			top += SHIFT_LINES;
			bottom += SHIFT_LINES;
			X86_COUNT(5);
		}
	}
	X86_COUNT(2);
	if (dst == 0) {
		X86_COUNT(2);
		if (SC(letterbox)) {
			wr32(slot_y, rd32(slot_y) + SHIFT_LINES);
			X86_COUNT(1);
		}
	}
	uint32_t dx = arg(0), dy = arg(1);
	cpu.r[ESP] -= 0x10;
	uint32_t rect = cpu.r[ESP];
	put_rect(rect, sx, top, sx + w, bottom);
	cursor_off(0);
	uint32_t dobj = surface_at(dst);
	X86_COUNT(13);
	bltfast(dobj, dx, dy, surface_at(src), rect, BLITFAST_WAIT);
	cpu.r[ECX] = rd32(dobj);
	cpu.r[EDX] = dx;
	cpu.r[ESP] += 0x10;
	cursor_on(2);
	X86_COUNT(2);
}

/* 0x4014f0: same arguments; on the screen the surface's colour key (8)
 * does the masking, otherwise the rectangle is copied by hand. */
void game_blit_masked(void)
{
	uint32_t sx = arg(2), sy = arg(3), w = arg(4), h = arg(5);
	uint32_t src = (arg(6) >> 8) & 0xff, dst = arg(6) & 0xff;
	X86_COUNT(9);
	if (dst == 0) {
		uint32_t top = sy, bottom = sy + h;
		uint32_t slot_y = cpu.r[ESP] + 4;
		int shift = 0;
		X86_COUNT(14);
		if (src == 0) {
			X86_COUNT(2);
			shift = SC(letterbox) != 0;
			if (shift) {
				top += SHIFT_LINES;
				bottom += SHIFT_LINES;
				X86_COUNT(5);
			}
		}
		if (src != 0 || shift) {
			X86_COUNT(2);
			if (SC(letterbox)) {
				wr32(slot_y, rd32(slot_y) + SHIFT_LINES);
				X86_COUNT(1);
			}
		}
		uint32_t dx = arg(0), dy = arg(1);
		cpu.r[ESP] -= 0x10;
		uint32_t rect = cpu.r[ESP];
		put_rect(rect, sx, top, sx + w, bottom);
		cursor_off(0);
		uint32_t screen = surface_at(0);
		X86_COUNT(13);
		bltfast(screen, dx, dy, surface_at(src), rect, BLITFAST_KEYED);
		cpu.r[ECX] = rd32(screen);
		cpu.r[EDX] = dx;
		cpu.r[ESP] += 0x10;
		cursor_on(0);
		X86_COUNT(4);
		return;
	}
	/* by hand: lock both, copy the rows, skipping colour 8 */
	uint32_t dx = arg(0), dy = arg(1);
	uint32_t sobj = surface_at(src), dobj = surface_at(dst);
	X86_COUNT(7);
	push32(sobj);
	game_lock_regs();
	cpu.r[ESP] += 4;
	uint32_t sp = cpu.r[EAX];
	X86_COUNT(6);
	push32(dobj);
	game_lock_regs();
	cpu.r[ESP] += 4;
	uint32_t dp = cpu.r[EAX];
	uint32_t spitch = rd32(PITCHES + 4 * src), dpitch = rd32(PITCHES + 4 * dst);
	sp += sy * spitch + sx;
	dp += dpitch * dy + dx;
	X86_COUNT(17);
	if ((int32_t)h > 0) {
		X86_COUNT(1);
		for (uint32_t r = 0; r < h; r++) {
			X86_COUNT(3);
			if ((int32_t)w > 0) {
				X86_COUNT(4);
				for (uint32_t i = 0; i < w; i++) {
					uint32_t v = rd8(sp + i);
					X86_COUNT(6);
					if (v != TRANSPARENT) {
						wr8(dp + i, v);
						X86_COUNT(1);
					}
				}
			}
			sp += rd32(PITCHES + 4 * src);
			dp += rd32(PITCHES + 4 * dst);
			X86_COUNT(8);
			if (r + 1 < h)
				X86_POLL(0x4015b1);
		}
	}
	X86_COUNT(4);
	push32(dobj);
	game_unlock_regs();
	cpu.r[ESP] += 4;
	cpu.r[ECX] = SURFACES + 4 * src;
	cpu.r[EDX] = sobj;
	X86_COUNT(4);
	push32(sobj);
	game_unlock_regs();
	cpu.r[ESP] += 4;
	X86_COUNT(1 + 4 + 2);
}

/* 0x401670: swap the rectangle (sx, sy, w, h) of surface surfaces >> 8
 * with the one at (dx, dy) of surface surfaces & 255, through the scratch
 * surface. */
void game_blit_swap(void)
{
	uint32_t dx = arg(0), dy = arg(1), sx = arg(2), sy = arg(3), w = arg(4), h = arg(5);
	uint32_t src = (arg(6) >> 8) & 0xff, dst = arg(6) & 0xff;
	uint32_t top = sy, bottom = sy + h, dtop = dy, dbottom = dy + h;
	X86_COUNT(38);
	if (src == 0) {
		X86_COUNT(2);
		if (SC(letterbox)) {
			top += SHIFT_LINES;
			bottom += SHIFT_LINES;
			X86_COUNT(5);
		}
	}
	X86_COUNT(2);
	if (dst == 0) {
		X86_COUNT(2);
		if (SC(letterbox)) {
			dtop += SHIFT_LINES;
			dbottom += SHIFT_LINES;
			X86_COUNT(5);
		}
	}
	cpu.r[ESP] -= 0x30;
	uint32_t r1 = cpu.r[ESP], r2 = r1 + 0x10, r3 = r1 + 0x20;
	put_rect(r1, sx, top, sx + w, bottom);
	put_rect(r2, dx, dtop, dx + w, dbottom);
	put_rect(r3, 0, 0, w, h);
	cursor_off(-1); /* cmp [cursor], edi; je */
	uint32_t scratch = SC(scratch_surface), sobj = surface_at(src), dobj = surface_at(dst);
	X86_COUNT(11);
	bltfast(scratch, 0, 0, sobj, r1, BLITFAST_WAIT);
	X86_COUNT(13);
	bltfast(sobj, sx, top, dobj, r2, BLITFAST_WAIT);
	cpu.r[ECX] = dx;
	X86_COUNT(13);
	bltfast(dobj, dx, dtop, scratch, r3, BLITFAST_WAIT);
	cpu.r[EDX] = rd32(dobj);
	cpu.r[ESP] += 0x30;
	cursor_on(4);
	X86_COUNT(2);
}

/* 0x4017b0 (cdecl x, y, w, h, colour << 4, surface): fill a rectangle
 * with colour (a Blt with DDBLT_COLORFILL). */
void game_fill_rect(void)
{
	uint32_t x = arg(0), y = arg(1), w = arg(2), h = arg(3), colour = arg(4), surf = arg(5);
	uint32_t top = y, bottom = y + h;
	X86_COUNT(15);
	if (surf == 0) {
		X86_COUNT(3);
		if (SC(letterbox)) {
			top += SHIFT_LINES;
			bottom += SHIFT_LINES;
			X86_COUNT(5);
		}
	}
	uint32_t sp_in = cpu.r[ESP];
	cpu.r[ESP] = sp_in - 4 - 0x74; /* the native frame, return address included */
	uint32_t rect = cpu.r[ESP], fx = rect + 0x10;
	put_rect(rect, x, top, x + w, bottom);
	for (uint32_t i = 0; i < 0x19; i++)
		wr32(fx + 4 * i, 0);
	wr32(fx, 0x64);
	wr32(fx + 0x50, (uint32_t)((int32_t)colour >> 4));
	X86_COUNT(5 + 0x19 + 4);
	cursor_off(1);
	uint32_t obj = surface_at(surf);
	push32(fx);
	push32(BLT_COLORFILL_WAIT);
	push32(0);
	push32(0);
	push32(rect);
	push32(obj);
	X86_COUNT(11);
	com(obj, COM_BLT, 6);
	cpu.r[ECX] = rd32(obj);
	cpu.r[EDX] = rect;
	cpu.r[ESP] = sp_in;
	cursor_on(1);
	X86_COUNT(2);
}

/* 0x401870 (cdecl x, y, w, h, colour << 4, surface): XOR the colour into
 * the pixels of a rectangle, four at a time (w is rounded up to a
 * multiple of 4). */
void game_xor_rect(void)
{
	uint32_t x = arg(0), y = arg(1), w = arg(2), h = arg(3), colour = arg(4), surf = arg(5);
	uint32_t c = colour >> 4, pat = c;
	pat = (pat << 8) + c;
	pat = (pat << 8) + c;
	pat = (pat << 8) + c;
	uint32_t obj = surface_at(surf);
	X86_COUNT(17);
	push32(obj);
	uint32_t p = game_surface_lock(obj);
	cpu.r[ESP] += 4;
	uint32_t pitch = rd32(PITCHES + 4 * surf);
	p += pitch * y + x;
	X86_COUNT(10);
	cpu.r[ECX] = h;
	if ((int32_t)h > 0) {
		X86_COUNT(4);
		for (uint32_t r = 0; r < h; r++) {
			X86_COUNT((r ? 1 : 0) + 2);
			if ((int32_t)w > 0) {
				for (uint32_t i = 0; (int32_t)i < (int32_t)w; i += 4) {
					wr32(p + i, rd32(p + i) ^ pat);
					X86_COUNT(4);
				}
			}
			p += rd32(PITCHES + 4 * surf);
			cpu.r[ECX] = rd32(PITCHES + 4 * surf);
			X86_COUNT(4);
			if (r + 1 < h)
				X86_POLL(0x4018e0);
		}
		X86_COUNT(1);
	}
	X86_COUNT(3);
	cpu.r[EDX] = obj;
	push32(obj);
	game_surface_unlock(obj);
	cpu.r[ESP] += 4;
	X86_COUNT(5);
}

void game_blit_copy_regs(void) { game_blit_copy(); }
void game_blit_masked_regs(void) { game_blit_masked(); }
void game_blit_swap_regs(void) { game_blit_swap(); }
void game_fill_rect_regs(void) { game_fill_rect(); }
void game_xor_rect_regs(void) { game_xor_rect(); }

void game_cursor_restore_regs(void) { game_cursor_restore(); }

/*
 * Picture load 0x406e20 (stage 5B). ECX non-zero: the name is argument 0
 * of the opcode, read into STRING_BUF and the file (a GP4 picture) loaded
 * to PICTURE; otherwise the picture already there is drawn again. The
 * animation slots are reset first. The picture: four u16 (x / 8, y, width
 * / 8, height) and, unless @[9] bit 0 keeps the palette, a palette block
 * (0x40752c), then the 8-bit pixels from PICTURE + 0x208; they go to the
 * rectangle (x, y, width, height) of the surface @[6] names, and the
 * rectangle is kept at PICTURE_RECT for the masked copy op 04 1b.
 */
#define PICTURE 0x4662beu
#define PICTURE_NAME 0x445d26u
#define PICTURE_RECT 0x44538au /* u16 x / 8 ... y, w / 8, h (4 x u16, 0x44538a-0x445390) */

void game_picture_load(void)
{
	uint32_t ecx = cpu.r[ECX];
	X86_COUNT(2);
	game_anim_reset();
	X86_COUNT(3);
	cpu.r[ECX] = ecx;
	if (ecx) {
		cpu.r[EDI] = PICTURE_NAME;
		game_string_arg_regs();
		push32(PICTURE);
		push32(PICTURE_NAME);
		X86_COUNT(6);
		game_file_load(PICTURE_NAME, PICTURE);
		cpu.r[ESP] += 8;
	}
	uint32_t esi = PICTURE;
	SV_SET(pic_rect[0], rd16(esi) >> 3);
	SV_SET(pic_rect[1], rd16(esi + 2));
	SV_SET(pic_rect[2], rd16(esi + 4) >> 3);
	SV_SET(pic_rect[3], rd16(esi + 6));
	X86_COUNT(15);
	if (!(SV(vflags) & 1)) {
		cpu.r[EAX] = esi + 8;
		X86_COUNT(2);
		game_palette_copy(cpu.r[EAX]);
	}
	esi += 0x208;
	uint32_t surf = SV(surface_select);
	push32(rd32(SURFACES + 4 * surf));
	game_lock_regs();
	cpu.r[ESP] += 4;
	uint32_t dst = cpu.r[EAX];
	uint32_t pitch = rd32(PITCHES + 4 * surf);
	cpu.r[EBX] = pitch;
	dst += SV(pic_rect[1]) * pitch + (SV(pic_rect[0]) << 3);
	uint32_t rows = SV(pic_rect[3]), dwords = SV(pic_rect[2]) << 1;
	X86_COUNT(18);
	cpu.r[EDX] = rows;
	cpu.r[ECX] = dwords;
	while (rows) {
		for (uint32_t i = 0; i < dwords; i++)
			wr32(dst + 4 * i, rd32(esi + 4 * i));
		esi += 4 * dwords;
		dst += pitch;
		rows--;
		X86_COUNT(7 + dwords);
		if (rows)
			X86_POLL(0x406ee1);
	}
	cpu.r[EDX] = 0;
	X86_COUNT(4);
	push32(rd32(SURFACES + 4 * surf));
	game_unlock_regs();
	cpu.r[ESP] += 4;
	X86_COUNT(4);
}
