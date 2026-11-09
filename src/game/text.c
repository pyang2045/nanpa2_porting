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

#define LOCK 0x401190u   /* cdecl (surface): pixels (hides the cursor on the screen) */
#define UNLOCK 0x4012b0u /* cdecl (surface) (redraws it) */
#define FILL 0x4017b0u   /* cdecl (x, y, w, h, colour << 4, surface) */

/* a stdcall import or COM method whose address is at slot */
static void stdcall_at(uint32_t slot, uint32_t nargs)
{
	game_ncall(rd32(slot), nargs);
}

/* 0x401900 (cdecl c, x, y, size, fg, bg): draw character c (Big5 if bit
 * 15 is set, high byte first; else ASCII) with its top left at pixel (x,
 * y) of the screen in font size & 3, as an opaque cell of N x N pixels,
 * N = 8 * (size & 3) + 16: fg where the glyph is, bg elsewhere. The cell
 * is copied from the scratch surface, whose rows are cleared again (32
 * bytes each) on the way. Out: EAX as the last unlock leaves it, EDX
 * the scratch surface unless that unlock redraws the cursor; ECX the character string (the native string buffer is ECX's save
 * slot: the bytes written over ECX's low ones). */
void game_draw_char(uint32_t c, uint32_t x, uint32_t y, uint32_t size, uint32_t fg, uint32_t bg)
{
	uint32_t esp = cpu.r[ESP];
	uint32_t str = esp - 4, dc_at = esp - 8;
	uint32_t len;
	wr32(str, cpu.r[ECX]);
	if (c & 0x8000) {
		wr8(str, c >> 8);
		wr8(str + 1, c);
		wr8(str + 2, 0);
		len = 2;
		X86_COUNT(14);
	} else {
		wr8(str, c);
		wr8(str + 1, 0);
		len = 1;
		X86_COUNT(10);
	}
	size &= 3;
	uint32_t n = 8 * size + 16;
	uint32_t scratch = SC(scratch_surface), vtbl = rd32(scratch);
	cpu.r[ESP] = dc_at;

	/* the glyph, white on black, at (0, 0) of the scratch surface */
	push32(dc_at);
	push32(scratch);
	X86_COUNT(9);
	stdcall_at(vtbl + SURFACE_GETDC, 2);
	uint32_t dc = rd32(dc_at);
	push32(rd32(FONTS + 4 * size));
	push32(dc);
	X86_COUNT(5);
	stdcall_at(IAT_SELECTOBJECT, 2);
	push32(0xf0f0f0);
	push32(dc);
	X86_COUNT(4);
	stdcall_at(IAT_SETTEXTCOLOR, 2);
	push32(0);
	push32(dc);
	X86_COUNT(4);
	stdcall_at(IAT_SETBKCOLOR, 2);
	push32(BK_OPAQUE);
	push32(dc);
	X86_COUNT(4);
	stdcall_at(IAT_SETBKMODE, 2);
	push32(len);
	push32(str);
	push32(0);
	push32(0);
	push32(dc);
	X86_COUNT(8);
	stdcall_at(IAT_TEXTOUT, 5);
	push32(dc);
	push32(scratch);
	X86_COUNT(6);
	stdcall_at(vtbl + SURFACE_RELEASEDC, 2);

	/* the cell, from the scratch surface to the screen */
	uint32_t screen = rd32(SURFACES);
	push32(scratch);
	X86_COUNT(3);
	game_lock_regs();
	uint32_t src = cpu.r[EAX];
	push32(screen);
	X86_COUNT(4);
	game_lock_regs();
	uint32_t dst = cpu.r[EAX] + rd32(PITCHES) * y + x;
	cpu.r[ESP] += 8;
	X86_COUNT(10);
	for (uint32_t row = 0; row < n; row++) {
		X86_COUNT(4);
		for (uint32_t col = 0; col < n; col++) {
			uint32_t on = rd8(src + col) != 0;
			wr8(dst + col, on ? fg : bg);
			X86_COUNT(on ? 7 : 8);
			if (col + 1 < n)
				X86_POLL(0x401a07);
		}
		for (uint32_t i = 0; i < 8; i++)
			wr32(src + 4 * i, 0);
		src += SCRATCH_PITCH;
		dst += rd32(PITCHES);
		X86_COUNT(11 + 8);
		if (row + 1 < n)
			X86_POLL(0x401a2b);
	}
	cpu.r[EDX] = scratch; /* 0x401a2d: mov edx, [0x4ab058]; push edx */
	push32(scratch);
	X86_COUNT(3);
	game_unlock_regs();
	cpu.r[EAX] = rd32(SURFACES); /* 0x401a39: mov eax, [0x44629e]; push eax */
	push32(cpu.r[EAX]);
	X86_COUNT(3);
	game_unlock_regs();
	cpu.r[ESP] = esp;
	cpu.r[ECX] = rd32(str);
	X86_COUNT(7);
}

void game_draw_char_regs(void)
{
	uint32_t sp = cpu.r[ESP];
	game_draw_char(rd32(sp), rd32(sp + 4), rd32(sp + 8), rd32(sp + 12), rd32(sp + 16),
	               rd32(sp + 20));
}

/* 0x406415: the cursor to the start of the next line, or of the window's
 * first line when the next one would not fit. AX, BX as natively (the
 * last value compared, the line height). */
void game_text_newline(void)
{
	uint32_t h = SV(text_line);
	SV_SET(text_x, SV(text_left));
	uint32_t y = (SV(text_y) + h) & 0xffff;
	SV_SET(text_y, y);
	set16(EBX, h);
	set16(EAX, y + h - 2);
	X86_COUNT(10);
	if ((cpu.r[EAX] & 0xffff) < SV(text_bottom)) {
		X86_COUNT(1);
		return;
	}
	set16(EAX, SV(text_top));
	SV_SET(text_y, cpu.r[EAX]);
	X86_COUNT(3);
}

/* 0x4063ee: the cursor past one character, to the next line when the
 * next one would not fit. */
void game_text_advance(void)
{
	uint32_t w = SV(text_advance);
	uint32_t x = (SV(text_x) + w) & 0xffff;
	SV_SET(text_x, x);
	set16(EBX, w);
	set16(EAX, x + w - 2);
	X86_COUNT(8);
	if ((cpu.r[EAX] & 0xffff) < SV(text_right)) {
		X86_COUNT(1);
		return;
	}
	game_text_newline();
}

/* 0x406b24: fill the text window on surface @[6] with colour EBX (the
 * fill routine's colour << 4) and home the cursor. EAX, ECX, EDX as the
 * fill leaves them. */
void game_clear_text_window(void)
{
	uint32_t a = SV(surface_select);
	push32(a);
	push32(cpu.r[EBX]);
	a = (a & 0xffff0000u) | ((SV(text_bottom) - SV(text_top) + 1) & 0xffff);
	push32(a);
	a = ((a & 0xffff0000u) | ((SV(text_right) - SV(text_left) + 1) & 0xffff)) << 3;
	push32(a);
	uint32_t top = SV(text_top), left = SV(text_left);
	a = (a & 0xffff0000u) | top;
	SV_SET(text_y, top);
	push32(a);
	a = ((a & 0xffff0000u) | left) << 3;
	SV_SET(text_x, left);
	push32(a);
	cpu.r[EAX] = a;
	X86_COUNT(20);
	game_fill_rect();
	cpu.r[ESP] += 0x18;
	X86_COUNT(2);
}
