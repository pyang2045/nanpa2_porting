/*
 * Opcode handlers that drive the other subsystems (jump units, stage 3):
 * text window, rectangles on the surfaces, the cursor, palette, pictures,
 * files, music and voice, animation slots, the map, and op 19's sub-ops.
 *
 * Each is the native handler's own work in C (argument parsing, the
 * argument registers and stack words, the branches between sub-ops) around
 * a call to the native routine that does the drawing or loading, which is
 * not reimplemented yet. Those calls run the lifted code (ncall below) with
 * the registers and stack words the native handler passes, so the callee
 * sees what it sees natively and leaves the CPU as it does: some handlers
 * store the callee's AX (op 19, ext 26).
 *
 * Sub-ops that wait on the timer or pump messages (op 19 sub-op 4 wipe,
 * op 1a sub-op 4, ext 16 mode 3, ext 26 sub-ops 5, 7 and 12), and any
 * whose sub-op is not a literal in the script, are left to the native
 * handler: their unit's `native` predicate (src/game/units.c) sends
 * hle_jump to the original, so they run exactly as before and are not
 * compared (the harness holds the timer off while it compares).
 *
 * Semantics: re-notes.md "Script opcodes".
 */
#include "game/file.h"
#include "game/map.h"
#include "game/vm.h"
#include "x86/x86.h"

/* ---- the native handler's register-level steps */

/* call 0x40741b: the argument list at ESI, registers as the parser leaves
 * them (ESI, EAX, ECX = count, EBX, EDI); returns EAX */
static uint32_t parse(void)
{
	game_parse_args_regs();
	X86_COUNT(1);
	return cpu.r[EAX];
}

/* `call addr` into native game code (or an HLE routine): the callee runs
 * on the emulated stack with the CPU as it is, and leaves it as it does.
 * Arguments are pushed by the caller, which also pops them (cdecl). None
 * of the callees used here can leave by the VM's stack reset (0x40638d). */
static void ncall(uint32_t addr)
{
	uint32_t esp = cpu.r[ESP];
	push32(X86_RET_SENTINEL);
	x86_exec(addr);
	if (cpu.r[ESP] != esp)
		x86_trap(addr, "native routine called by an opcode handler did not return");
}

static void drop(uint32_t words)
{
	cpu.r[ESP] += 4 * words;
	X86_COUNT(1);
}

/* EAX with its low word replaced (`mov ax, ...`) */
static uint32_t ax_(uint32_t eax, uint32_t v)
{
	return (eax & 0xffff0000u) | (v & 0xffffu);
}

/* `jmp 0x4060f9` at native_pc (backward: polls), ESI as it is */
static uint32_t loop_at(uint32_t native_pc)
{
	X86_POLL(native_pc);
	return VM_LOOP;
}

/* argument values (0x445d06 + 2i) */
#define ARG(i) (VM_ARG_VALUES + 2u * (i))
#define CURSOR_SHOWN 0x445ce0u   /* u8: the game cursor is drawn (ext 15) */
#define SURFACE_SELECT 0x4452dau /* u16 @[6]: surface the text window and rect ops use */
#define FRAME 0x446292u          /* u32: frames since start (timer, 60 Hz) */
#define STOPWATCH 0x446296u      /* u32: frame of the last op 19 2 x 0 */
#define FADE_STEPS 0x44628eu     /* u32: palette fade length (ext 16 mode 3 sets 1) */
#define VM_FLAGS9 0x4452e0u      /* u8 @[9]: bit 0 keep palette on picture load */
#define STRING_BUF 0x445d26u     /* file names for ext 1c, 1d, 25 */

/* The rectangle arguments x0 y0 x1 y1 (args 0-3, x in units of 8 pixels)
 * pushed as the native code does: height, width * 8, y0, x0 * 8, all built
 * in AX over the upper half EAX already has (the `shl eax, 3` carries bits
 * into it). Returns EAX. */
static uint32_t push_rect(uint32_t eax)
{
	eax = ax_(eax, rd16(ARG(3)) - rd16(ARG(1)) + 1);
	push32(eax);
	eax = ax_(eax, rd16(ARG(2)) - rd16(ARG(0)) + 1) << 3;
	push32(eax);
	eax = ax_(eax, rd16(ARG(1)));
	push32(eax);
	eax = ax_(eax, rd16(ARG(0))) << 3;
	push32(eax);
	return eax;
}

/* ---- the native-handler predicates (units.c) */

/* Argument 0 is a single constant followed by the expression end 03:
 * *v = its value. Only 30-3f, `07 b` and `08 h l`; anything else counts
 * as not literal. Reads the script only (no time charged). */
static int literal_arg0(uint32_t *v)
{
	uint32_t pc = cpu.r[ESI], b = vm_script_byte(pc);
	if (b >= 0x30 && b <= 0x3f) {
		*v = b - 0x30;
		pc += 1;
	} else if (b == 0x07) {
		*v = vm_script_byte(pc + 1) >> 2;
		pc += 2;
	} else if (b == 0x08) {
		*v = (vm_script_byte(pc + 1) >> 2) << 6 | vm_script_byte(pc + 2) >> 2;
		pc += 3;
	} else {
		return 0;
	}
	return vm_script_byte(pc) == 0x03;
}

int game_native_misc(void) /* 19: sub-op 4 is the dissolve wipe */
{
	uint32_t v;
	return !literal_arg0(&v) || v == 4 || v > 5;
}

/* ---- the handlers */

/* 17 [c]: clear the text window (0x406b24) in colour c (c << 4 as the
 * fill routine wants it), or in the text colour with no argument. */
uint32_t game_op_clear_text(void)
{
	uint32_t a = parse();
	cpu.r[EBX] = rd16(VM_TEXT_COLOUR);
	if (cpu.r[ECX])
		cpu.r[EBX] = cpu.r[EAX] = a << 4;
	X86_COUNT(5);
	game_clear_text_window();
	return loop_at(0x406b1f);
}

/* 04 17 / 04 18 x0 y0 x1 y1: fill (0x4017b0) or XOR (0x401870) the
 * rectangle of surface @[6] with the text colour. */
static uint32_t colour_rect(void (*fn)(void), uint32_t exit_pc)
{
	parse();
	uint32_t eax = rd16(SURFACE_SELECT);
	push32(eax);
	eax = ax_(eax, rd16(VM_TEXT_COLOUR));
	push32(eax);
	push_rect(eax);
	X86_COUNT(20);
	fn();
	drop(6);
	return loop_at(exit_pc);
}

uint32_t game_op_fill_rect(void)
{
	return colour_rect(game_fill_rect, 0x406bc6);
}

uint32_t game_op_xor_rect(void)
{
	return colour_rect(game_xor_rect, 0x406c1b);
}

/* 04 19 / 04 1a / 04 1b x0 y0 x1 y1 s dx dy d: copy (0x401430), swap
 * (0x401670) or masked copy (0x4014f0) the rectangle of surface s to (dx,
 * dy) of surface d. The two surface numbers go in one word (s << 8 | d,
 * the low bytes of args 4 and 7). */
static void push_blit(void)
{
	uint32_t eax = rd8(ARG(4)) << 8 | rd8(ARG(7));
	push32(eax);
	eax = push_rect(eax);
	eax = ax_(eax, rd16(ARG(6)));
	push32(eax);
	eax = ax_(eax, rd16(ARG(5))) << 3;
	push32(eax);
	X86_COUNT(24);
}

static uint32_t blit(void (*fn)(void), uint32_t exit_pc)
{
	parse();
	push_blit();
	fn();
	drop(7);
	return loop_at(exit_pc);
}

uint32_t game_op_copy_rect(void)
{
	return blit(game_blit_copy, 0x406c80);
}

uint32_t game_op_swap_rect(void)
{
	return blit(game_blit_swap, 0x406ce5);
}

/* With no arguments 04 1b draws the last picture's rectangle (0x44538a-
 * 0x445390, set by 0x406e20) onto itself, surfaces 1 -> 2. */
uint32_t game_op_masked_rect(void)
{
	parse();
	if (cpu.r[ECX]) {
		push_blit();
		game_blit_masked();
		drop(7);
		return loop_at(0x406d4e);
	}
	uint32_t eax = 0x102;
	push32(eax);
	eax = ax_(eax, rd16(0x445390));
	push32(eax);
	eax = ax_(eax, rd16(0x44538e)) << 3;
	push32(eax);
	eax = ax_(eax, rd16(0x44538c));
	push32(eax);
	uint32_t ecx = ax_(cpu.r[ECX], rd16(0x44538a)) << 3;
	push32(ecx);
	push32(eax);
	push32(ecx);
	cpu.r[EAX] = eax;
	cpu.r[ECX] = ecx;
	X86_COUNT(16);
	game_blit_masked();
	drop(7);
	return loop_at(0x406d86);
}

/* 04 14 n: with n = 1, count the visible items of the menu list at
 * 0x445e0c (the label ext 13 set) into 0x445302; otherwise fill the item
 * table at @[22] (0x4452fa, an offset in segment 0) with {text offset,
 * item number} pairs: the first [0x445300] visible items after skipping
 * [0x4452fe]. The table is cleared first ([0x445300] dwords). The item
 * step 0x4068e1 is game_menu_item_regs; its AL is the item's guard value
 * (low byte), -1 at the list's end. */
#define MENU_START 0x445e0cu
#define MENU_ROWS 0x445300u  /* u16: rows the table holds */
#define MENU_SKIP 0x4452feu  /* u8: visible items to skip (scroll) */
#define MENU_COUNT 0x445302u /* u16: visible items (n = 1) */

static void menu_item(void)
{
	game_menu_item_regs();
	X86_COUNT(2);
}

uint32_t game_op_menu_items(void)
{
	uint32_t a = parse(), esi = cpu.r[ESI];
	if (a == 1) {
		uint32_t n = 0;
		cpu.r[ESI] = rd16(MENU_START);
		X86_COUNT(6);
		for (;;) {
			menu_item();
			uint32_t al = cpu.r[EAX] & 0xff;
			X86_COUNT(5);
			if (al == 0xff)
				break;
			if (al != 0)
				n++;
			X86_POLL(al ? 0x4068d2 : 0x4068cf);
		}
		wr16(MENU_COUNT, n);
		cpu.r[ECX] = n;
		cpu.r[ESI] = esi;
		X86_COUNT(3);
		return loop_at(0x4068dc);
	}
	uint32_t rows = rd16(MENU_ROWS);
	uint32_t table = VM_SCRIPT + rd16(VM_MENU_LIST_PTR);
	for (uint32_t i = 0; i < rows; i++)
		wr32(table + 4 * i, 0);
	X86_COUNT(9 + rows);
	cpu.r[EAX] = 0;
	uint8_t left = (uint8_t)rd8(MENU_ROWS), skip = (uint8_t)rd8(MENU_SKIP);
	uint32_t item = 0, edi = VM_SCRIPT + rd16(VM_MENU_LIST_PTR);
	cpu.r[ESI] = rd16(MENU_START);
	for (;;) {
		item++;
		menu_item();
		uint32_t al = cpu.r[EAX] & 0xff;
		X86_COUNT(10);
		if (al == 0xff)
			break;
		if (al == 0) {
			X86_POLL(0x406899);
			continue;
		}
		if (skip) {
			skip--;
			X86_POLL(0x4068a2);
			continue;
		}
		wr16(edi, cpu.r[EBX]);
		wr16(edi + 2, item);
		edi += 4;
		X86_COUNT(4);
		if (!--left)
			break;
		X86_POLL(0x4068b0);
	}
	cpu.r[ECX] = (uint32_t)skip << 8 | left;
	cpu.r[EDX] = item;
	cpu.r[EDI] = edi;
	cpu.r[ESI] = esi;
	return loop_at(0x4068b3);
}

/* 04 15 n ...: the game cursor. 0 show (0x401c40) and 1 hide (0x401c60),
 * each only if not already so; 2 nothing; 3 x, y: move the mouse
 * (0x401c80); 4 off: load the 32x32 AND/XOR cursor bitmap (128 dwords,
 * byte-swapped) from offset off of segment @[7] to 0x4be900. */
uint32_t game_op_cursor(void)
{
	uint32_t a = parse();
	switch (a) {
	case 0:
		X86_COUNT(3);
		if (rd8(CURSOR_SHOWN))
			return loop_at(0x40693d);
		game_cursor_show();
		wr8(CURSOR_SHOWN, 1);
		X86_COUNT(1);
		return loop_at(0x40694f);
	case 1:
		X86_COUNT(3);
		if (!rd8(CURSOR_SHOWN))
			return loop_at(0x40695b);
		game_cursor_hide();
		wr8(CURSOR_SHOWN, 0);
		X86_COUNT(1);
		return loop_at(0x40696d);
	case 2:
		X86_COUNT(1);
		return VM_LOOP; /* the table entry itself: no backward jump */
	case 3: {
		uint32_t eax = rd16(ARG(2));
		push32(eax);
		eax = ax_(eax, rd16(ARG(1)));
		push32(eax);
		cpu.r[EAX] = eax;
		X86_COUNT(6);
		game_cursor_setpos();
		drop(2);
		return loop_at(0x406989);
	}
	case 4: {
		uint32_t src = vm_segment_base(vm_segment_select()) + rd16(ARG(1)), dst = 0x4be900, v = 0;
		X86_COUNT(9);
		for (int i = 0; i < 0x80; i++) {
			v = rd32(src);
			v = v >> 24 | (v >> 8 & 0xff00u) | (v << 8 & 0xff0000u) | v << 24;
			wr32(dst, v);
			src += 4;
			dst += 4;
			X86_COUNT(4);
			if (i < 0x7f)
				X86_POLL(0x4069b5);
		}
		cpu.r[EAX] = v;
		cpu.r[ECX] = 0;
		X86_COUNT(2);
		return loop_at(0x4069b9);
	}
	}
	x86_trap(0x40691b, "op 04 15: sub-op out of the table");
}

/* 04 16 n [v]: the palette. 2: set it now (0x401ca0) from the picture's
 * palette, or with v from all entries = v (0x40754d); the tables are saved
 * before (0x40756c) and restored after (0x407599). 3: the same, but fade
 * to it (0x401d30) and wait until the timer has finished the fade
 * (game_palette_wait_piece, 0x406f41); others do nothing. */
uint32_t game_op_palette(void)
{
	uint32_t a = parse();
	X86_COUNT(a == 2 ? 2 : 4); /* the compares with 2 and 3 */
	if (a != 2 && a != 3)
		return loop_at(0x406f13);
	X86_COUNT(1); /* call 0x40756c */
	game_palette_save();
	X86_COUNT(2);
	if (rd8(VM_ARG_TYPES + 1) == VM_ARG_EXPR) {
		cpu.r[EAX] = ax_(cpu.r[EAX], rd16(ARG(1)));
		X86_COUNT(2);
		game_palette_fill(cpu.r[EAX]);
	}
	if (a == 3) {
		wr32(FADE_STEPS, 1);
		X86_COUNT(2);
		game_fade_start();
		return 0x406f41;
	}
	X86_COUNT(2);
	game_palette_upload();
	game_palette_restore();
	return loop_at(0x406f77);
}

/* 0x406f41: while the fade runs the native code asks again (a backward
 * jump: game_jump polls for the piece that returns itself); then it puts
 * the tables back. */
uint32_t game_palette_wait_piece(void)
{
	cpu.r[EAX] = game_fade_active();
	X86_COUNT(3);
	if (cpu.r[EAX])
		return 0x406f41;
	game_palette_restore();
	X86_COUNT(1);
	return loop_at(0x406f4f);
}

/* 04 1c name, off: load a file into segment @[7] at off (0x401050). */
uint32_t game_op_load_file(void)
{
	parse();
	cpu.r[EDI] = STRING_BUF;
	game_string_arg_regs();
	uint32_t eax = vm_segment_base(vm_segment_select()) + rd16(ARG(1));
	cpu.r[EAX] = eax;
	cpu.r[EBX] = rd16(ARG(1));
	push32(eax);
	push32(STRING_BUF);
	X86_COUNT(9);
	game_file_load(rd32(cpu.r[ESP]), rd32(cpu.r[ESP] + 4));
	drop(2);
	return loop_at(0x406dbf);
}

/* 04 1d [name]: picture to surface @[6] (0x406e20; with no argument the
 * one already in 0x4662be). */
uint32_t game_op_picture(void)
{
	parse();
	game_picture_load();
	return loop_at(0x406e1b);
}

/* 04 24 n, m: n = 0 plays music m (0x402cb0), anything else stops it
 * (0x4032c0). */
uint32_t game_op_music(void)
{
	uint32_t a = parse();
	X86_COUNT(2);
	if (a) {
		ncall(0x4032c0);
		return loop_at(0x4073b4);
	}
	cpu.r[EAX] = ax_(a, rd16(ARG(1)));
	push32(cpu.r[EAX]);
	X86_COUNT(2);
	ncall(0x402cb0);
	drop(1);
	return loop_at(0x4073c8);
}

/* 04 25 name: play a voice clip (0x403360). */
uint32_t game_op_voice(void)
{
	parse();
	cpu.r[EDI] = STRING_BUF;
	game_string_arg_regs();
	push32(STRING_BUF);
	X86_COUNT(3);
	ncall(0x403360);
	drop(1);
	return loop_at(0x4073e9);
}

/* 1a n, slot, a, b: animation slot control 0x403510 (EAX sub-op, EBX
 * slot, ECX, EDX), a call exit (0x4070b1, return 0x4070b6): the control
 * is the jump unit game_anim_control_piece, which for sub-op 4 goes on in
 * the wait loop 0x4035b5 until the slot has stopped. */
uint32_t game_op_anim(void)
{
	parse();
	push32(cpu.r[ESI]);
	cpu.r[EBX] = rd16(ARG(1));
	cpu.r[ECX] = rd16(ARG(2));
	cpu.r[EDX] = rd16(ARG(3));
	push32(0x4070b6);
	X86_COUNT(5);
	return 0x403510;
}

uint32_t game_op_anim_done(void) /* 0x4070b6 */
{
	cpu.r[ESI] = pop32();
	X86_COUNT(1);
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
 *   4: dissolve wipe (the native handler's);
 *   5 a: S = a. */
static uint32_t misc_restore(void)
{
	uint32_t esi = rd32(VM_SEGMENTS + 12), edi = 0x4a62be;
	uint32_t n = rd16(esi);
	esi += 2;
	for (uint32_t i = 0; i < n; i++)
		wr8(edi++, rd8(esi++));
	uint32_t w = rd16(esi + 4), ecx = rd16(esi + 6) * w + 0x208;
	edi = 0x4662be + ecx - 1;
	esi = esi + ecx - 1;
	X86_COUNT(15 + n + ecx);
	for (uint32_t i = 0; i < ecx; i++)
		wr8(edi--, rd8(esi--));
	uint32_t edx = ax_(cpu.r[EDX], rd16(SURFACE_SELECT));
	wr16(SURFACE_SELECT, 1);
	wr8(VM_FLAGS9, rd8(VM_FLAGS9) | 1);
	cpu.r[EAX] = w;
	cpu.r[ESI] = esi;
	cpu.r[EDI] = edi;
	cpu.r[ECX] = 0;
	cpu.r[EDX] = edx;
	push32(edx);
	X86_COUNT(7);
	game_picture_load();
	cpu.r[EDX] = pop32();
	wr8(VM_FLAGS9, rd8(VM_FLAGS9) & 0xfe);
	wr16(SURFACE_SELECT, cpu.r[EDX]);
	X86_COUNT(4);
	return cpu.r[EAX];
}

static uint32_t misc_row_swap(void)
{
	uint32_t y = rd16(ARG(1));
	uint64_t m0 = (uint64_t)y * rd32(0x4462ae), m1 = (uint64_t)y * rd32(0x4462b2);
	uint32_t off0 = (uint32_t)m0 + 0x40, off1 = (uint32_t)m1 + 0x40;
	cpu.r[EDX] = (uint32_t)(m1 >> 32);
	push32(off0);
	push32(off1);
	push32(rd32(0x44629e));
	cpu.r[EAX] = off1;
	X86_COUNT(9);
	game_lock_regs();
	drop(1);
	uint32_t esi = cpu.r[EAX];
	push32(rd32(0x4462a2));
	X86_COUNT(2);
	game_lock_regs();
	drop(1);
	uint32_t edi = cpu.r[EAX];
	edi += pop32();
	esi += pop32();
	X86_COUNT(5);
	uint32_t a = 0, b = 0;
	for (int i = 0; i < 0x80; i++) {
		a = rd32(esi);
		b = rd32(edi);
		wr32(esi, b);
		wr32(edi, a);
		esi += 4;
		edi += 4;
		X86_COUNT(7);
		if (i < 0x7f)
			X86_POLL(0x40735f);
	}
	cpu.r[ESI] = esi;
	cpu.r[EDI] = edi;
	cpu.r[EBX] = b;
	cpu.r[ECX] = 0;
	push32(rd32(0x44629e));
	X86_COUNT(2);
	game_unlock_regs();
	drop(1);
	push32(rd32(0x4462a2));
	X86_COUNT(2);
	game_unlock_regs();
	drop(1);
	return cpu.r[EAX];
}

uint32_t game_op_misc(void)
{
	uint32_t n = parse(), eax;
	uint32_t esi = cpu.r[ESI];
	cpu.r[ESI] = ARG(1); /* the sub-ops address their arguments from ESI */
	X86_COUNT(3);
	switch (n) {
	case 0:
		eax = misc_restore();
		break;
	case 1:
		eax = ax_(cpu.r[EAX], 0xff);
		X86_COUNT(2);
		break;
	case 2: {
		uint32_t a = (rd16(ARG(1)) - 1) & 0xffff;
		eax = ax_(cpu.r[EAX], a);
		X86_COUNT(3);
		if (a & 0x8000) { /* js: a was 0 (or above 0x8000) */
			cpu.r[EAX] = eax;
			game_fade_start();
			eax = cpu.r[EAX];
			X86_COUNT(1);
		} else if (a == 0) {
			X86_COUNT(2);
			if (rd16(ARG(2)) == 1) {
				eax = rd32(FRAME) - rd32(STOPWATCH);
				X86_COUNT(3);
			} else {
				cpu.r[EAX] = eax;
				game_fade_active();
				eax = cpu.r[EAX];
				X86_COUNT(1);
			}
		} else {
			uint32_t b = rd16(ARG(2));
			cpu.r[EBX] = b;
			X86_COUNT(2);
			if (b == 0) {
				eax = rd32(FRAME);
				wr32(STOPWATCH, eax);
				X86_COUNT(3);
			} else if (b == 1) {
				eax = rd32(FADE_STEPS);
				X86_COUNT(2);
			} else if (b == 2) {
				X86_COUNT(1);
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
		eax = ax_(cpu.r[EAX], rd16(ARG(1)));
		X86_COUNT(2);
		break;
	default: /* 4 and out of the table: game_native_misc */
		x86_trap(0x40720f, "op 19: sub-op left to the native handler reached C");
	}
	vm_set_reg(VM_REG_S, eax);
	cpu.r[EAX] = eax;
	cpu.r[ESI] = esi;
	X86_COUNT(2);
	return loop_at(0x407235);
}
