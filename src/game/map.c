/*
 * The town map, part 1 (stage 5A): the 04 26 dispatcher 0x403a70 and the
 * sub-ops that only move data around (init, sprite scripts, entry points,
 * layer unpacking and copying), the area trigger test and the sprite
 * collision test. The drawing is in map_draw.c, the movement in
 * map_move.c, the walk planner in map_path.c.
 *
 * 04 26 n, args: the VM's ext op 0x26 (src/game/ops_sys.c game_op_map)
 * parses the argument list, calls 0x403a70 with ESI = the argument values
 * (0x445d06), and keeps AX as the script register S. 0x403a70 copies
 * arguments 1..6 to MAP_ARGS (the sub-ops read them there) and runs
 * sub-op n of a table the native code holds in .data; the C dispatches on
 * n itself. Sub-ops 5, 7 and 12 pump messages and stay native until the
 * pump is split (docs/reimpl.md "Stage 5A"); so does any sub-op not
 * listed here.
 *
 * The result of a sub-op is AX at its return, whatever it was last used
 * for (the unit comparison sees it as S), so each function below leaves
 * AX as the native one does.
 */
#include "game/map.h"

/* ---- the sub-ops */

/* Sub-op 0, 0x403a87: take the layer's size, find the walk grid after the
 * layer (w*h tiles of 2 bytes, 4 bytes of size before) and forget the
 * event state. S = the walk grid's offset. */
static uint32_t op_init(void)
{
	uint32_t m = map_base();
	uint32_t layer = rd16(m + M_LAYER);
	uint32_t size = rd32(SEG1 + layer);
	wr32(m + M_MAP_W, size);
	uint32_t w = rd16(m + M_MAP_W), h = rd16(m + M_MAP_H);
	uint32_t grid = ((w * h) * 2 + 4 + rd16(m + M_LAYER)) & 0xffff;
	wr16(MAP_WALK_BASE, grid);
	wr8(MAP_AUTOWALK, 0);
	wr16(MAP_LAST_SPRITE, 0xffff);
	wr16(MAP_LAST_CLICK, 0xffff);
	wr16(MAP_LAST_AREA, 0xffff);
	X86_COUNT(26);
	return (size & 0xffff0000u) | grid; /* EAX: the size's height over the offset */
}

/* Sub-op 1, 0x403ae9: patch the first sprite script (the player's): bytes
 * 0xc1 and 0xc3 at 2 and 4. S = 1 (EAX is left at the sub-op number). */
static void op_player_script(void)
{
	uint32_t m = map_base();
	uint32_t table = rd16(m + M_SCRIPTS);
	uint32_t script = rd16(SEG1 + table);
	wr8(SEG1 + script + 2, 0xc1);
	wr8(SEG1 + script + 4, 0xc3);
	wr16(MAP_LAST_SPRITE, 0xffff);
	wr16(MAP_LAST_CLICK, 0xffff);
	wr16(MAP_LAST_AREA, 0xffff);
	X86_COUNT(15);
}

/* the sprite table entry of MAP_ARGS[0] (EDI = M, ESI = the sprite) */
static uint32_t sprite_arg(uint32_t m)
{
	uint32_t n = rd16(MAP_ARGS);
	return ((n << 4) + rd16(m + M_SPRITES)) & 0xffff;
}

/* Sub-op 13, 0x4053a6: sprite a runs script b from its start, with no
 * movement in progress. S = b. */
static uint32_t op_sprite_script(void)
{
	uint32_t s = SEG1 + sprite_arg(map_base());
	uint32_t b = rd16(MAP_ARGS + 2);
	wr8(s + S_SCRIPT, b);
	wr8(s + S_SCRIPT_PC, 0);
	wr16(s + S_KIND, 0); /* kind and count */
	X86_COUNT(16);
	return b;
}

/* Sub-op 14, 0x4053de: b = 0 enables sprite a (clears the off flag), else
 * disables it. S = b. */
static uint32_t op_sprite_enable(void)
{
	uint32_t s = SEG1 + sprite_arg(map_base());
	uint32_t b = rd16(MAP_ARGS + 2);
	if (b == 0)
		wr8(s + S_FLAGS, rd8(s + S_FLAGS) & ~SPRITE_OFF);
	else
		wr8(s + S_FLAGS, rd8(s + S_FLAGS) | SPRITE_OFF);
	X86_COUNT(15);
	return b;
}

/* The entry n, 18 bytes: the view cell, the player's cell and frame, the
 * scroll limits (M_MIN_X..M_MAX_Y) */
static uint32_t entry_arg(uint32_t m)
{
	uint32_t e = rd16(m + M_ENTRIES);
	uint32_t n2 = rd16(MAP_ARGS) * 2;
	return SEG1 + e + n2 * 8 + n2;
}

static void history_fill(void)
{
	for (uint32_t i = 1; i < HISTORY_LEN; i++) {
		wr32(HISTORY + 5 * i, rd32(HISTORY));
		wr8(HISTORY + 5 * i + 4, rd8(HISTORY + 4));
		X86_POLL(0x4050f6);
	}
	X86_COUNT(5 + 9 * 6);
}

/* Sub-op 10, 0x40502c: put the view and the first three sprites at entry
 * a, fill the history with that position and take the entry's scroll
 * limits. Stops the auto-walk. S = the entry's last limit word. */
static uint32_t op_enter(void)
{
	wr8(MAP_AUTOWALK, 0);
	uint32_t m = map_base();
	uint32_t e = entry_arg(m);
	wr16(m + M_VIEW_X, rd16(e));
	wr16(m + M_VIEW_Y, rd16(e + 2));
	uint32_t sp = SEG1 + rd16(m + M_SPRITES);
	uint32_t x = rd16(e + 4);
	wr16(sp + S_X, x);
	wr16(sp + 0x10 + S_X, x);
	wr16(sp + 0x20 + S_X, x);
	wr16(HISTORY + 1, x);
	uint32_t y = rd16(e + 6);
	wr16(sp + S_Y, y);
	wr16(sp + 0x10 + S_Y, y);
	wr16(sp + 0x20 + S_Y, y);
	wr16(HISTORY + 3, y);
	uint32_t f = rd16(e + 8) & 0xff;
	wr8(sp + S_FRAME, f);
	wr8(sp + 0x10 + S_FRAME, f);
	wr8(sp + 0x20 + S_FRAME, f);
	wr8(HISTORY, f);
	history_fill();
	wr32(m + M_MIN_X, rd32(e + 0xa));
	uint32_t last = rd32(e + 0xe);
	wr32(m + M_MAX_X, last);
	X86_COUNT(60);
	return last;
}

/* Sub-op 11, 0x405113: the same from the player's own position (the view
 * is not moved, the walk is not stopped). */
static uint32_t op_reset_history(void)
{
	uint32_t m = map_base();
	uint32_t e = entry_arg(m);
	uint32_t sp = SEG1 + rd16(m + M_SPRITES);
	uint32_t x = rd16(sp + S_X);
	wr16(sp + 0x10 + S_X, x);
	wr16(sp + 0x20 + S_X, x);
	wr16(HISTORY + 1, x);
	uint32_t y = rd16(sp + S_Y);
	wr16(sp + 0x10 + S_Y, y);
	wr16(sp + 0x20 + S_Y, y);
	wr16(HISTORY + 3, y);
	uint32_t f = rd8(sp + S_FRAME);
	wr8(sp + 0x10 + S_FRAME, f);
	wr8(sp + 0x20 + S_FRAME, f);
	wr8(HISTORY, f);
	history_fill();
	wr32(m + M_MIN_X, rd32(e + 0xa));
	uint32_t last = rd32(e + 0xe);
	wr32(m + M_MAX_X, last);
	X86_COUNT(55);
	return last;
}

/* Sub-op 15, 0x405415: unpack the run-length coded layer at segment
 * offset a over the current layer. Words are tiles; with bit 14 set a
 * word is repeated as many times as the byte after it says. S = the last
 * word stored. */
static uint32_t op_unpack(void)
{
	uint32_t m = map_base();
	uint32_t dst = rd16(m + M_LAYER);
	uint32_t src = rd16(MAP_ARGS);
	uint32_t w = rd16(SEG1 + src), h = rd16(SEG1 + src + 2);
	wr16(SEG1 + dst, w);
	wr16(SEG1 + dst + 2, h);
	if (!w || !h)
		x86_trap(0x405415, "map: unpacking a layer with no cells");
	src += 4;
	dst += 4;
	uint32_t run = 0, v = 0;
	X86_COUNT(20);
	for (uint32_t n = w * h; n; n--) {
		if (run) {
			X86_COUNT(6);
			run--;
		} else {
			v = rd16(SEG1 + src);
			src += 2;
			if (v & 0x4000) {
				v &= 0xbfff;
				run = rd8(SEG1 + src) - 1; /* 0 wraps: a run of 4G */
				src++;
				X86_COUNT(14);
			} else {
				X86_COUNT(10);
			}
		}
		wr16(SEG1 + dst, v);
		dst += 2;
		X86_POLL(0x405487);
	}
	return v;
}

/* Sub-op 16, 0x40548f: copy the rectangle of layer cells (a, b)-(c, d) to
 * (e, f), row by row, forwards. S = the last cell copied. */
static uint32_t op_copy_cells(void)
{
	uint32_t m = map_base();
	uint32_t layer = rd16(m + M_LAYER);
	uint32_t w = rd16(SEG1 + layer);
	uint32_t stride = w * 2;
	uint32_t a = rd16(MAP_ARGS), b = rd16(MAP_ARGS + 2), c = rd16(MAP_ARGS + 4), d = rd16(MAP_ARGS + 6);
	uint32_t e = rd16(MAP_ARGS + 8), f = rd16(MAP_ARGS + 10);
	uint32_t src = (((b * w) & 0xffff) + a) * 2 + 4 + layer;
	uint32_t dst = (((f * w) & 0xffff) + e) * 2 + 4 + layer;
	src &= 0xffff;
	dst &= 0xffff;
	uint32_t rows = (d - b + 1) & 0xffff, cols = (c - a + 1) & 0xffff;
	if (!rows || !cols)
		x86_trap(0x40548f, "map: copying an empty rectangle");
	uint32_t v = 0;
	X86_COUNT(40);
	for (uint32_t r = 0; r < rows; r++) {
		uint32_t s = src, t = dst;
		X86_COUNT(10);
		for (uint32_t k = 0; k < cols; k++) {
			v = rd16(SEG1 + s);
			wr16(SEG1 + t, v);
			s += 2;
			t += 2;
			X86_COUNT(6);
			X86_POLL(0x405536);
		}
		src += stride;
		dst += stride;
		X86_POLL(0x40553f);
	}
	return v;
}

/* Sub-op 32, 0x40558f: stop the auto-walk. S = 32. */
static void op_stop_walk(void)
{
	wr8(MAP_AUTOWALK, 0);
	X86_COUNT(2);
}

/* ---- area trigger and collision */

/* Sub-op 8, 0x404f8c: the area table entry (x0, y0, x1, y1, code; 10 bytes
 * each, ended by x0 = 0xffff) that contains sprite a's cell; AX = its
 * code or 0xffff. Called by the event check. EBX = the last coordinate
 * compared. */
void game_map_area_trigger(void)
{
	uint32_t sel = rd16(MAP_SEL);
	uint32_t areas = rd16(sel + SEG1 + M_AREAS);
	uint32_t sprites = rd16(sel + SEG1 + M_SPRITES);
	uint32_t s = ((((rd16(MAP_ARGS) & 0xffff) << 4) & 0xffff) + sprites) & 0xffff;
	uint32_t cx = rd16(SEG1 + s + S_X), dx = rd16(SEG1 + s + S_Y);
	uint32_t bx, ax;
	X86_COUNT(10);
	for (;;) {
		bx = rd16(SEG1 + areas);
		ax = 0xffff;
		X86_COUNT(4);
		if (bx == 0xffff) {
			X86_COUNT(1);
			break;
		}
		X86_COUNT(bx == cx ? 2 : 3);
		if (bx <= cx) {
			bx = rd16(SEG1 + areas + 4);
			X86_COUNT(3);
			if (bx >= cx) {
				bx = rd16(SEG1 + areas + 2);
				X86_COUNT(bx == dx ? 3 : 4);
				if (bx <= dx) {
					bx = rd16(SEG1 + areas + 6);
					X86_COUNT(3);
					if (bx >= dx) {
						ax = rd16(SEG1 + areas + 8);
						X86_COUNT(2);
						break;
					}
				}
			}
		}
		areas += 10;
		X86_COUNT(2);
		X86_POLL(0x405004);
	}
	set16(EAX, ax);
	cpu.r[EBX] = bx; /* bx replaces the low word of the movzx'd selector */
	set16(ECX, cx);
	set16(EDX, dx);
	cpu.r[ESI] = s;
	cpu.r[EDI] = areas;
}

/* 0x404e25: does sprite s (an offset in segment 1) collide with another
 * sprite at the cell it is about to enter? The cell is one step ahead in
 * its facing (frame >> 1: up, down, left, right); sprites with a footprint
 * span their width or height along the other axis. Skips itself, sprites
 * 1 and 2 (the player's followers), the ones flagged ghost, solid-not, off
 * or not drawn. Returns the sprite's index or -1; AX/flags as the native
 * code leaves them, and the other registers it uses. */
int game_map_collide(uint32_t s)
{
	uint32_t sp = SEG1 + s;
	X86_COUNT(2);
	if (rd8(sp + S_FLAGS) & SPRITE_GHOST) {
		set16(EAX, 0xffff);
		(void)f_sub(0xffff, 0xffff, 0, 16);
		X86_COUNT(3);
		return -1;
	}
	uint32_t dir = (rd8(sp + S_FRAME) >> 1) & 0xff;
	set8l(EAX, dir);
	wr8(MAP_DIR, dir);
	uint32_t sel = rd16(MAP_SEL);
	uint32_t base = rd16(SEG1 + sel + M_SPRITES);
	uint32_t cx = rd16(sp + S_X), dx = rd16(sp + S_Y);
	uint32_t bx = cpu.r[EBX];
	X86_COUNT(9);
	if (dir == 0) {
		dx = (dx - 1) & 0xffff;
		X86_COUNT(4);
	} else if (dir == 1) {
		bx = rd8(sp + S_HEIGHT);
		dx = (dx + bx) & 0xffff;
		X86_COUNT(8);
	} else if (dir == 2) {
		cx = (cx - 1) & 0xffff;
		X86_COUNT(8);
	} else {
		bx = rd8(sp + S_WIDTH);
		cx = (cx + bx) & 0xffff;
		X86_COUNT(9);
	}
	uint32_t o = base, ax = 0;
	int found = -1;
	X86_COUNT(2);
	if (rd8(SEG1 + o) == SPRITE_END) {
		ax = 0xffff;
		X86_COUNT(2 + 4);
		goto done;
	}
	X86_COUNT(1);
	for (;;) {
		if (s == o) {
			X86_COUNT(5);
		} else if (ax == 1) {
			X86_COUNT(7);
		} else if (ax == 2) {
			X86_COUNT(9);
		} else {
			uint32_t fl = rd8(SEG1 + o);
			X86_COUNT(6 + 2);
			if (fl == SPRITE_END) {
				ax = 0xffff;
				X86_COUNT(2 + 4);
				break;
			}
			if (fl & SPRITE_SOLID_NOT) {
				X86_COUNT(2 + 3);
			} else if (fl & SPRITE_OFF) {
				X86_COUNT(2 + 2 + 3);
			} else if (!(fl & SPRITE_DRAWN)) {
				X86_COUNT(2 + 2 + 2 + 3);
			} else {
				X86_COUNT(2 + 2 + 2);
				bx = rd16(SEG1 + o + S_X);
				uint32_t cxa = cx;
				X86_COUNT(4);
				if (dir < 2) {
					cxa = (cx + rd8(sp + S_WIDTH) - 1) & 0xffff;
					X86_COUNT(3);
				}
				X86_COUNT(bx == cxa ? 3 : 4);
				if (bx <= cxa) {
					bx = (bx + rd8(SEG1 + o + S_WIDTH) - 1) & 0xffff;
					X86_COUNT(7 + 2);
					/* bx + width - 1 >= cx */
					if (!(bx < cx)) {
						bx = rd16(SEG1 + o + S_Y);
						uint32_t dxa = dx;
						X86_COUNT(4);
						if (dir >= 2) {
							dxa = (dx + rd8(sp + S_HEIGHT) - 1) & 0xffff;
							X86_COUNT(3);
						}
						X86_COUNT(bx == dxa ? 3 : 4);
						if (bx <= dxa) {
							bx = (rd16(SEG1 + o + S_Y) + rd8(SEG1 + o + S_HEIGHT) - 1) & 0xffff;
							X86_COUNT(7 + 2);
							if (bx >= dx) {
								found = (int)ax;
								X86_COUNT(4);
								goto fin;
							}
							X86_COUNT(3);
							goto next;
						}
						X86_COUNT(3);
						goto next;
					}
					X86_COUNT(3);
					goto next;
				}
				X86_COUNT(3);
				goto next;
			}
		}
next:
		o += SPRITE_SIZE;
		ax++;
		X86_POLL(0x404e9f);
	}
done:
	X86_COUNT(0);
fin:
	set16(EAX, ax);
	set16(ECX, cx);
	set16(EDX, dx);
	set16(EBX, bx);
	(void)f_sub(ax, 0xffff, 0, 16);
	return found;
}

/* Sub-op 9, 0x40500e: collision test for sprite a. AX = the index of the
 * sprite in the way, or 0xffff. */
void game_map_collision(void)
{
	uint32_t sel = rd16(MAP_SEL);
	uint32_t n16 = rd16(MAP_ARGS) << 4;
	uint32_t s = (n16 & 0xffff0000u) | ((n16 + rd16(sel + SEG1 + M_SPRITES)) & 0xffff);
	cpu.r[EDI] = sel;
	cpu.r[ESI] = s;
	X86_COUNT(6); /* movzx, movzx, shl, add, call, ret */
	game_map_collide(s);
}

/* ---- the dispatcher */

/* The sub-ops the C handles; the rest (5, 7, 12, which pump messages, and
 * the sub-ops no script uses) are the native handler's. */
int game_map_op_in_c(uint32_t op)
{
	switch (op) {
	case 0: case 1: case 2: case 3: case 4: case 6: case 8: case 9: case 10: case 11:
	case 13: case 14: case 15: case 16: case 30: case 32:
		return 1;
	default:
		return 0;
	}
}

/* 0x403a70 for the C handler of 04 26 (ops_sys.c game_op_map): EAX = the
 * sub-op, ESI = the argument values. Copies arguments 1..6 to MAP_ARGS and
 * runs the sub-op; AX is its result. */
void game_map_dispatch(void)
{
	uint32_t op = cpu.r[EAX];
	uint32_t src = cpu.r[ESI] + 2;
	for (uint32_t i = 0; i < 3; i++)
		wr32(MAP_ARGS + 4 * i, rd32(src + 4 * i));
	cpu.r[ESI] = src + 12;
	cpu.r[EDI] = MAP_ARGS + 12;
	cpu.r[ECX] = 0;
	X86_COUNT(9);
	switch (op) {
	case 0:
		cpu.r[EAX] = op_init();
		break;
	case 1:
		op_player_script();
		break;
	case 2:
		game_map_fill_grid();
		break;
	case 3:
		game_map_overlay_sprites();
		break;
	case 4:
		game_map_redraw_changed();
		break;
	case 6: /* 0x4042e7 */
		game_map_fill_grid();
		game_map_overlay_sprites();
		game_map_redraw_changed();
		X86_COUNT(4);
		break;
	case 8:
		game_map_area_trigger();
		break;
	case 9:
		game_map_collision();
		break;
	case 10:
		cpu.r[EAX] = op_enter();
		break;
	case 11:
		cpu.r[EAX] = op_reset_history();
		break;
	case 13:
		cpu.r[EAX] = op_sprite_script();
		break;
	case 14:
		cpu.r[EAX] = op_sprite_enable();
		break;
	case 15:
		cpu.r[EAX] = op_unpack();
		break;
	case 16:
		cpu.r[EAX] = op_copy_cells();
		break;
	case 30:
		cpu.r[EAX] = game_map_op_walk();
		break;
	case 32:
		op_stop_walk();
		break;
	default:
		x86_trap(0x403a7f, "map sub-op left to the native code reached C");
	}
	X86_COUNT(2);
}
