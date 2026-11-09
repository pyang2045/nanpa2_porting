/*
 * Opcode handlers that drive the other subsystems (R2: direct style): text
 * window, rectangles on the surfaces, the cursor, palette, pictures, files,
 * music and voice, animation slots, and op 19's sub-ops.
 *
 * Each is the native handler's own work in C (argument parsing, the branches
 * between sub-ops) around the C routine that does the drawing or loading
 * (screen.c, file.c, anim.c, wipe.c), with the routine's typed arguments
 * where the native handler pushed words. Contract as the other handlers
 * (ops.c): entered with vm_pc past the opcode bytes, they return -1 (on with
 * the loop). The routines that wait (the wipe, the fade wait, the animation
 * slot wait) are plain calls: they pump through ctl_pump() themselves.
 *
 * Semantics: re-notes.md "Script opcodes".
 */
#include "game/ctl.h"
#include "game/file.h"
#include "game/map.h"
#include "game/vm.h"
#include "x86/x86.h"
#include "game/state.h"

/* call 0x40741b: the argument list at vm_pc; returns argument 0's value (the
 * native EAX), *count = the number of arguments (the native ECX) */
static uint32_t parse(uint32_t *count)
{
	uint32_t a, n = vm_parse_args(&vm_pc, &a);
	if (count)
		*count = n;
	return a;
}

/* EAX with its low word replaced (`mov ax, ...`) */
static uint32_t ax_(uint32_t eax, uint32_t v)
{
	return (eax & 0xffff0000u) | (v & 0xffffu);
}

/* `jmp 0x4060f9` at native_pc */
static int loop_at(uint32_t native_pc)
{
	return -1;
}

/* argument values (0x445d06 + 2i) */
#define ARG(i) (VM_ARG_VALUES + 2u * (i))
#define STRING_BUF 0x445d26u     /* file names for ext 1c, 1d, 25 */

/* The rectangle arguments x0 y0 x1 y1 (args 0-3, x in units of 8 pixels)
 * as the native code builds them to push: r = { x0 * 8, y0, width * 8,
 * height }, each built in AX over the upper half EAX already has (the
 * `shl eax, 3` carries bits into it). All 32 bits are real (the rectangle
 * routines store them whole). Returns EAX. */
static uint32_t push_rect(uint32_t eax, uint32_t r[4])
{
	eax = ax_(eax, rd16(ARG(3)) - rd16(ARG(1)) + 1);
	r[3] = eax;
	eax = ax_(eax, rd16(ARG(2)) - rd16(ARG(0)) + 1) << 3;
	r[2] = eax;
	eax = ax_(eax, rd16(ARG(1)));
	r[1] = eax;
	eax = ax_(eax, rd16(ARG(0))) << 3;
	r[0] = eax;
	return eax;
}

/* ---- the handlers */

/* 17 [c]: clear the text window (0x406b24) in colour c (c << 4 as the
 * fill routine wants it), or in the text colour with no argument. */
int vm_op_clear_text(void)
{
	uint32_t n, a = parse(&n);
	uint32_t colour = SV(text_colour);
	if (n)
		colour = a << 4;
	game_clear_text_window(colour);
	return loop_at(0x406b1f);
}

/* 04 17 / 04 18 x0 y0 x1 y1: fill (0x4017b0) or XOR (0x401870) the
 * rectangle of surface @[6] with the text colour. */
static int colour_rect(void (*fn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t),
                       uint32_t exit_pc)
{
	parse(NULL);
	uint32_t surf = SV(surface_select);
	uint32_t colour = ax_(surf, SV(text_colour)), r[4];
	push_rect(colour, r);
	fn(r[0], r[1], r[2], r[3], colour, surf);
	return loop_at(exit_pc);
}

int vm_op_fill_rect(void)
{
	return colour_rect(game_fill_rect, 0x406bc6);
}

int vm_op_xor_rect(void)
{
	return colour_rect(game_xor_rect, 0x406c1b);
}

/* 04 19 / 04 1a / 04 1b x0 y0 x1 y1 s dx dy d: copy (0x401430), swap
 * (0x401670) or masked copy (0x4014f0) the rectangle of surface s to (dx,
 * dy) of surface d. The two surface numbers go in one word (s << 8 | d,
 * the low bytes of args 4 and 7). */
typedef void (*blit_fn)(uint32_t dx, uint32_t dy, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                        uint8_t src, uint8_t dst);

/* the blit's arguments as the native handler builds them (dx, dy, sx, sy,
 * w, h are the 32-bit words it pushes) */
static void run_blit(blit_fn fn)
{
	uint32_t eax = rd8(ARG(4)) << 8 | rd8(ARG(7)), r[4];
	eax = push_rect(eax, r);
	eax = ax_(eax, rd16(ARG(6)));
	uint32_t dy = eax;
	eax = ax_(eax, rd16(ARG(5))) << 3;
	fn(eax, dy, r[0], r[1], r[2], r[3], rd8(ARG(4)), rd8(ARG(7)));
}

static int blit(blit_fn fn, uint32_t exit_pc)
{
	parse(NULL);
	run_blit(fn);
	return loop_at(exit_pc);
}

int vm_op_copy_rect(void)
{
	return blit(game_blit_copy, 0x406c80);
}

int vm_op_swap_rect(void)
{
	return blit(game_blit_swap, 0x406ce5);
}

/* With no arguments 04 1b draws the last picture's rectangle (0x44538a-
 * 0x445390, set by 0x406e20) onto itself, surfaces 1 -> 2. */
int vm_op_masked_rect(void)
{
	uint32_t n;
	parse(&n);
	if (n) {
		run_blit(game_blit_masked);
		return loop_at(0x406d4e);
	}
	uint32_t eax = 0x102;
	eax = ax_(eax, SV(pic_rect[3]));
	uint32_t h = eax;
	eax = ax_(eax, SV(pic_rect[2])) << 3;
	uint32_t w = eax;
	eax = ax_(eax, SV(pic_rect[1]));
	uint32_t ecx = ax_(0 /* the count n = 0 */, SV(pic_rect[0])) << 3;
	game_blit_masked(ecx, eax, ecx, eax, w, h, 1, 2);
	return loop_at(0x406d86);
}

/* 04 14 n: with n = 1, count the visible items of the menu list at
 * 0x445e0c (the label ext 13 set) into 0x445302; otherwise fill the item
 * table at @[22] (0x4452fa, an offset in segment 0) with {text offset,
 * item number} pairs: the first [0x445300] visible items after skipping
 * [0x4452fe]. The table is cleared first ([0x445300] dwords). The item
 * step 0x4068e1 is vm_menu_item; its result is the item's guard value
 * (low byte), 0xff at the list's end. */
#define MENU_START 0x445e0cu

int vm_op_menu_items(void)
{
	uint32_t a = parse(NULL), pc, text;
	if (a == 1) {
		uint32_t n = 0;
		pc = rd16(MENU_START);
		for (;;) {
			uint32_t al = vm_menu_item(&pc, &text);
			if (al == 0xff)
				break;
			if (al != 0)
				n++;
		}
		SV_SET(menu_count, n);
		return loop_at(0x4068dc);
	}
	uint32_t rows = SV(menu_rows);
	uint32_t table = VM_SCRIPT + SV(menu_list_ptr);
	for (uint32_t i = 0; i < rows; i++)
		wr32(table + 4 * i, 0);
	uint8_t left = (uint8_t)SV(menu_rows), skip = (uint8_t)SV(menu_skip);
	uint32_t item = 0, edi = VM_SCRIPT + SV(menu_list_ptr);
	pc = rd16(MENU_START);
	for (;;) {
		item++;
		uint32_t al = vm_menu_item(&pc, &text);
		if (al == 0xff)
			break;
		if (al == 0) {
			continue;
		}
		if (skip) {
			skip--;
			continue;
		}
		wr16(edi, text);
		wr16(edi + 2, item);
		edi += 4;
		if (!--left)
			break;
	}
	return loop_at(0x4068b3);
}

/* 04 15 n ...: the game cursor. 0 show (0x401c40) and 1 hide (0x401c60),
 * each only if not already so; 2 nothing; 3 x, y: move the mouse
 * (0x401c80); 4 off: load the 32x32 AND/XOR cursor bitmap (128 dwords,
 * byte-swapped) from offset off of segment @[7] to 0x4be900. */
int vm_op_cursor(void)
{
	uint32_t a = parse(NULL);
	switch (a) {
	case 0:
		if (RT(cursor_shown))
			return loop_at(0x40693d);
		game_cursor_show();
		RT_SET(cursor_shown, 1);
		return loop_at(0x40694f);
	case 1:
		if (!RT(cursor_shown))
			return loop_at(0x40695b);
		game_cursor_hide();
		RT_SET(cursor_shown, 0);
		return loop_at(0x40696d);
	case 2:
		return -1; /* the table entry itself: no backward jump */
	case 3:
		game_cursor_setpos(rd16(ARG(1)), rd16(ARG(2)));
		return loop_at(0x406989);
	case 4: {
		uint32_t src = vm_segment_base(vm_segment_select()) + rd16(ARG(1)), dst = 0, v;
		for (int i = 0; i < 0x80; i++) {
			v = rd32(src);
			v = v >> 24 | (v >> 8 & 0xff00u) | (v << 8 & 0xff0000u) | v << 24;
			memcpy((uint8_t *)&G.cursor_bits + dst, &v, 4);
			src += 4;
			dst += 4;
		}
		return loop_at(0x4069b9);
	}
	}
	x86_trap(0x40691b, "op 04 15: sub-op out of the table");
}

/* 04 16 n [v]: the palette. 2: set it now (0x401ca0) from the picture's
 * palette, or with v from all entries = v (0x40754d); the tables are saved
 * before (0x40756c) and restored after (0x407599). 3: the same, but fade
 * to it (0x401d30) and wait until the timer has finished the fade
 * (game_palette_wait, 0x406f41); others do nothing. */
int vm_op_palette(void)
{
	uint32_t a = parse(NULL);
	if (a != 2 && a != 3)
		return loop_at(0x406f13);
	game_palette_save();
	if (rd8(VM_ARG_TYPES + 1) == VM_ARG_EXPR) {
		game_palette_fill(rd16(ARG(1)));
	}
	if (a == 3) {
		RT_SET(fade_steps, 1);
		game_fade_start();
		game_palette_wait(); /* 0x406f41 */
		return loop_at(0x406f4f);
	}
	game_palette_upload();
	game_palette_restore();
	return loop_at(0x406f77);
}

/* 04 1c name, off: load a file into segment @[7] at off (0x401050). */
int vm_op_load_file(void)
{
	parse(NULL);
	uint32_t src, last;
	vm_string_arg(STRING_BUF, &src, &last);
	uint32_t buf = vm_segment_base(vm_segment_select()) + rd16(ARG(1));
	game_file_load_at(STRING_BUF, buf);
	return loop_at(0x406dbf);
}

/* 04 1d [name]: picture to surface @[6] (0x406e20; with no argument the
 * one already in 0x4662be). */
int vm_op_picture(void)
{
	uint32_t n;
	parse(&n);
	game_picture_load(n != 0);
	return loop_at(0x406e1b);
}

/* 04 24 n, m: n = 0 plays music m (0x402cb0), anything else stops it
 * (0x4032c0). */
int vm_op_music(void)
{
	uint32_t a = parse(NULL);
	if (a) {
		HLE0(0x4032c0);
		return loop_at(0x4073b4);
	}
	HLE(0x402cb0, rd16(ARG(1)));
	return loop_at(0x4073c8);
}

/* 04 25 name: play a voice clip (0x403360). */
int vm_op_voice(void)
{
	parse(NULL);
	uint32_t src, last;
	vm_string_arg(STRING_BUF, &src, &last);
	HLE(0x403360, STRING_BUF);
	return loop_at(0x4073e9);
}

/* 1a n, slot, a, b: animation slot control 0x403510 (sub-op n, the slot and
 * two operands), run directly (anim.c): sub-op 4 waits in its loop until the
 * slot has stopped (that loop does not pump). */
int vm_op_anim(void)
{
	uint32_t a = parse(NULL);
	game_anim_control(a, rd16(ARG(1)), rd16(ARG(2)), rd16(ARG(3)));
	return loop_at(0x4070b7);
}

/* 19 n, a, b: sub-ops, the result in S.
 *   0: restore the picture stashed in segment 3 (palette words, then the
 *      picture block copied backwards to 0x4662be) and draw it to surface
 *      1 with the palette kept (0x406e20, @[9] bit 0 set meanwhile);
 *   1: S = 0xff;
 *   2 a b: a = 0 start a palette fade (0x401d30); a = 1: b = 1 frames
 *      since the stopwatch, else whether a fade runs (0x401e10); other a:
 *      b = 0 start the stopwatch, 1 the fade length, 2 S = a - 1;
 *   3 y: swap 512 bytes of row y + 0x40 bytes between surfaces 0 and 1;
 *   4: dissolve wipe (wipe.c);
 *   5 a: S = a. */
static uint32_t misc_restore(void)
{
	uint32_t esi = G.save.n.segment[3], edi = 0x4a62be;
	uint32_t n = rd16(esi);
	esi += 2;
	for (uint32_t i = 0; i < n; i++)
		wr8(edi++, rd8(esi++));
	uint32_t w = rd16(esi + 4), ecx = rd16(esi + 6) * w + 0x208;
	edi = 0x4662be + ecx - 1;
	esi = esi + ecx - 1;
	for (uint32_t i = 0; i < ecx; i++)
		wr8(edi--, rd8(esi--));
	uint32_t surface = SV(surface_select);
	SV_SET(surface_select, 1);
	SV_SET(vflags, SV(vflags) | 1);
	uint32_t eax = game_picture_load(0);
	SV_SET(vflags, SV(vflags) & 0xfe);
	SV_SET(surface_select, surface);
	return eax;
}

static uint32_t misc_row_swap(void)
{
	uint32_t y = rd16(ARG(1));
	uint64_t m0 = (uint64_t)y * rd32(0x4462ae), m1 = (uint64_t)y * rd32(0x4462b2);
	uint32_t off0 = (uint32_t)m0 + 0x40, off1 = (uint32_t)m1 + 0x40;
	uint32_t esi = game_surface_lock(rd32(0x44629e));
	uint32_t edi = game_surface_lock(rd32(0x4462a2));
	edi += off1;
	esi += off0;
	uint8_t *pa = x86_ptr(esi, 0x200), *pb = x86_ptr(edi, 0x200);
	for (int i = 0; i < 0x80; i++) {
		uint32_t a, b;
		memcpy(&a, pa + 4 * i, 4);
		memcpy(&b, pb + 4 * i, 4);
		memcpy(pa + 4 * i, &b, 4);
		memcpy(pb + 4 * i, &a, 4);
	}
	game_surface_unlock(rd32(0x44629e));
	uint32_t r = game_surface_unlock(rd32(0x4462a2));
	return r;
}

int vm_op_misc(void)
{
	uint32_t n = parse(NULL), eax = n;
	switch (n) {
	case 0:
		eax = misc_restore();
		break;
	case 1:
		eax = ax_(eax, 0xff);
		break;
	case 2: {
		uint32_t a = (rd16(ARG(1)) - 1) & 0xffff;
		eax = ax_(eax, a);
		if (a & 0x8000) { /* js: a was 0 (or above 0x8000) */
			eax = game_fade_start();
		} else if (a == 0) {
			if (rd16(ARG(2)) == 1) {
				eax = RT(frame) - RT(stopwatch);
			} else {
				eax = game_fade_active();
			}
		} else {
			uint32_t b = rd16(ARG(2));
			if (b == 0) {
				eax = RT(frame);
				RT_SET(stopwatch, eax);
			} else if (b == 1) {
				eax = RT(fade_steps);
			} else if (b == 2) {
			} else {
				x86_trap(0x4072b0, "op 19 2: sub-op out of the table");
			}
		}
		break;
	}
	case 3:
		eax = misc_row_swap();
		break;
	case 5:
		eax = ax_(eax, rd16(ARG(1)));
		break;
	case 4: /* the dissolve wipe: push esi (saved), 2, 0 and call 0x401a50 */
		eax = game_wipe(0, 2);
		vm_set_reg(VM_REG_S, eax);
		return loop_at(0x407235);
	default:
		x86_trap(0x40720f, "op 19: sub-op out of the table");
	}
	vm_set_reg(VM_REG_S, eax);
	return loop_at(0x407235);
}
