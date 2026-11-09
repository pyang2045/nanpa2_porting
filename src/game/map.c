/*
 * The town map, part 1: the sub-ops of 04 26 that only move data around (init,
 * sprite scripts, entry points, layer unpacking and copying), the area trigger
 * test and the sprite collision test, and the dispatch of the sub-ops that do
 * not wait. The drawing is in map_draw.c, the movement in map_move.c, the walk
 * planner in map_path.c, the sprite scripts in map_sprite.c, and the sub-ops
 * that wait for the window (5, 7, 12) and the handler itself in map_pump.c.
 *
 * 04 26 n, args: the handler (map_pump.c) parses the argument list; 0x403a70
 * copies arguments 1..6 to G.map.v.args (the sub-ops read them there) and runs
 * sub-op n. The script register S gets the AX the sub-op leaves, whatever it
 * was last used for, so each function below returns the AX the native one
 * leaves (only the low 16 bits are kept).
 */
#include "game/map.h"

/* ---- the sub-ops */

/* Sub-op 0, 0x403a87: take the layer's size, find the walk grid after the
 * layer (w*h tiles of 2 bytes, 4 bytes of size before) and forget the
 * event state. AX = the walk grid's offset. */
static uint32_t op_init(void)
{
	struct map_rec *m = map_rec();
	uint32_t layer = m->layer;
	uint32_t size = seg1_u16(layer) | (seg1_u16(layer + 2) << 16);
	m->map_w = size & 0xffff;
	m->map_h = size >> 16;
	uint32_t grid = ((m->map_w * m->map_h) * 2 + 4 + m->layer) & 0xffff;
	MV_SET(walk_base, grid);
	MV_SET(autowalk, 0);
	MV_SET(last_sprite, 0xffff);
	MV_SET(last_click, 0xffff);
	MV_SET(last_area, 0xffff);
	return (size & 0xffff0000u) | grid; /* EAX: the size's height over the offset */
}

/* Sub-op 1, 0x403ae9: patch the first sprite script (the player's): bytes
 * 0xc1 and 0xc3 at 2 and 4. AX stays at the sub-op number. */
static void op_player_script(void)
{
	struct map_rec *m = map_rec();
	uint32_t script = seg1_u16(m->scripts);
	seg1_set8(script + 2, 0xc1);
	seg1_set8(script + 4, 0xc3);
	MV_SET(last_sprite, 0xffff);
	MV_SET(last_click, 0xffff);
	MV_SET(last_area, 0xffff);
}

/* the sprite table entry of argument 1 */
static struct sprite *sprite_arg(const struct map_rec *m)
{
	return sprite_at(((MV(args[0]) << 4) + m->sprites) & 0xffff);
}

/* Sub-op 13, 0x4053a6: sprite a runs script b from its start, with no
 * movement in progress. AX = b. */
static uint32_t op_sprite_script(void)
{
	struct sprite *s = sprite_arg(map_rec());
	uint32_t b = MV(args[1]);
	s->script = b;
	s->script_pc = 0;
	s->kind = 0;
	s->count = 0;
	return b;
}

/* Sub-op 14, 0x4053de: b = 0 enables sprite a (clears the off flag), else
 * disables it. AX = b. */
static uint32_t op_sprite_enable(void)
{
	struct sprite *s = sprite_arg(map_rec());
	uint32_t b = MV(args[1]);
	if (b == 0)
		s->flags &= (uint8_t)~SPRITE_OFF;
	else
		s->flags |= SPRITE_OFF;
	return b;
}

/* The entry of argument 1, 18 bytes: the view cell, the player's cell and
 * frame, the scroll limits */
static struct entry *entry_arg(const struct map_rec *m)
{
	uint32_t n2 = MV(args[0]) * 2;
	return entry_at(m->entries + n2 * 8 + n2);
}

static void history_fill(void)
{
	for (uint32_t i = 1; i < HISTORY_LEN; i++) {
		*hist(i) = *hist(0);
	}
}

/* Sub-op 10, 0x40502c: put the view and the first three sprites at entry
 * a, fill the history with that position and take the entry's scroll
 * limits. Stops the auto-walk. AX = the entry's last limit word pair. */
static uint32_t op_enter(void)
{
	MV_SET(autowalk, 0);
	struct map_rec *m = map_rec();
	struct entry *e = entry_arg(m);
	m->view_x = e->view_x;
	m->view_y = e->view_y;
	struct sprite *sp = sprites_of(m);
	uint32_t x = e->x;
	sp[0].x = sp[1].x = sp[2].x = x;
	hist(0)->x = x;
	uint32_t y = e->y;
	sp[0].y = sp[1].y = sp[2].y = y;
	hist(0)->y = y;
	uint32_t f = e->frame & 0xff;
	sp[0].frame = sp[1].frame = sp[2].frame = f;
	hist(0)->frame = f;
	history_fill();
	m->min_x = e->min_x;
	m->min_y = e->min_y;
	uint32_t last = e->max_x | ((uint32_t)e->max_y << 16);
	m->max_x = e->max_x;
	m->max_y = e->max_y;
	return last;
}

/* Sub-op 11, 0x405113: the same from the player's own position (the view
 * is not moved, the walk is not stopped). */
static uint32_t op_reset_history(void)
{
	struct map_rec *m = map_rec();
	struct entry *e = entry_arg(m);
	struct sprite *sp = sprites_of(m);
	uint32_t x = sp[0].x;
	sp[1].x = sp[2].x = x;
	hist(0)->x = x;
	uint32_t y = sp[0].y;
	sp[1].y = sp[2].y = y;
	hist(0)->y = y;
	uint32_t f = sp[0].frame;
	sp[1].frame = sp[2].frame = f;
	hist(0)->frame = f;
	history_fill();
	m->min_x = e->min_x;
	m->min_y = e->min_y;
	uint32_t last = e->max_x | ((uint32_t)e->max_y << 16);
	m->max_x = e->max_x;
	m->max_y = e->max_y;
	return last;
}

/* Sub-op 15, 0x405415: unpack the run-length coded layer at segment
 * offset a over the current layer. Words are tiles; with bit 14 set a
 * word is repeated as many times as the byte after it says. AX = the last
 * word stored. */
static uint32_t op_unpack(void)
{
	struct map_rec *m = map_rec();
	uint32_t dst = m->layer;
	uint32_t src = MV(args[0]);
	uint32_t w = seg1_u16(src), h = seg1_u16(src + 2);
	seg1_set16(dst, w);
	seg1_set16(dst + 2, h);
	if (!w || !h)
		game_trap(0x405415, "map: unpacking a layer with no cells");
	src += 4;
	dst += 4;
	uint32_t run = 0, v = 0;
	for (uint32_t n = w * h; n; n--) {
		if (run) {
			run--;
		} else {
			v = seg1_u16(src);
			src += 2;
			if (v & 0x4000) {
				v &= 0xbfff;
				run = seg1_u8(src) - 1; /* 0 wraps: a run of 4G */
				src++;
			} else {
			}
		}
		seg1_set16(dst, v);
		dst += 2;
	}
	return v;
}

/* Sub-op 16, 0x40548f: copy the rectangle of layer cells (a, b)-(c, d) to
 * (e, f), row by row, forwards. AX = the last cell copied. */
static uint32_t op_copy_cells(void)
{
	struct map_rec *m = map_rec();
	uint32_t layer = m->layer;
	uint32_t w = seg1_u16(layer);
	uint32_t stride = w * 2;
	uint32_t a = MV(args[0]), b = MV(args[1]), c = MV(args[2]), d = MV(args[3]);
	uint32_t e = MV(args[4]), f = MV(args[5]);
	uint32_t src = (((b * w) & 0xffff) + a) * 2 + 4 + layer;
	uint32_t dst = (((f * w) & 0xffff) + e) * 2 + 4 + layer;
	src &= 0xffff;
	dst &= 0xffff;
	uint32_t rows = (d - b + 1) & 0xffff, cols = (c - a + 1) & 0xffff;
	if (!rows || !cols)
		game_trap(0x40548f, "map: copying an empty rectangle");
	uint32_t v = 0;
	for (uint32_t r = 0; r < rows; r++) {
		uint32_t s = src, t = dst;
		for (uint32_t k = 0; k < cols; k++) {
			v = seg1_u16(s);
			seg1_set16(t, v);
			s += 2;
			t += 2;
		}
		src += stride;
		dst += stride;
	}
	return v;
}

/* Sub-op 32, 0x40558f: stop the auto-walk. */
static void op_stop_walk(void)
{
	MV_SET(autowalk, 0);
}

/* ---- area trigger and collision */

/* Sub-op 8, 0x404f8c: the area table entry (x0, y0, x1, y1, code; 10 bytes
 * each, ended by x0 = 0xffff) that contains sprite n's cell; AX = its
 * code or 0xffff. Called by the event check. */
uint32_t game_map_area_trigger(uint32_t n)
{
	const struct map_rec *m = map_rec();
	uint32_t areas = m->areas;
	uint32_t off = ((((n & 0xffff) << 4) & 0xffff) + m->sprites) & 0xffff;
	const struct sprite *s = sprite_at(off);
	uint32_t cx = s->x, dx = s->y;
	uint32_t ax;
	for (;;) {
		const struct area *a = area_at(areas);
		uint32_t bx = a->x0;
		ax = 0xffff;
		if (bx == 0xffff) {
			break;
		}
		if (bx <= cx) {
			bx = a->x1;
			if (bx >= cx) {
				bx = a->y0;
				if (bx <= dx) {
					bx = a->y1;
					if (bx >= dx) {
						ax = a->code;
						break;
					}
				}
			}
		}
		areas += 10;
	}
	return ax;
}

/* 0x404e25: does sprite s collide with another sprite at the cell it is
 * about to enter? The cell is one step ahead in its facing (frame >> 1: up,
 * down, left, right); sprites with a footprint span their width or height
 * along the other axis. Skips itself, sprites 1 and 2 (the player's
 * followers), the ones flagged ghost, solid-not, off or not drawn. Returns
 * the sprite's index or -1. */
int game_map_collide(const struct sprite *s)
{
	if (s->flags & SPRITE_GHOST) {
		return -1;
	}
	uint32_t dir = (s->frame >> 1) & 0xff;
	MV_SET(dir, dir);
	const struct map_rec *m = map_rec();
	const struct sprite *o = sprites_of(m);
	uint32_t cx = s->x, dx = s->y;
	if (dir == 0) {
		dx = (dx - 1) & 0xffff;
	} else if (dir == 1) {
		dx = (dx + s->height) & 0xffff;
	} else if (dir == 2) {
		cx = (cx - 1) & 0xffff;
	} else {
		cx = (cx + s->width) & 0xffff;
	}
	uint32_t ax = 0;
	if (o->flags == SPRITE_END) {
		return -1;
	}
	for (;;) {
		if (s == o) {
		} else if (ax == 1) {
		} else if (ax == 2) {
		} else {
			uint32_t fl = o->flags;
			if (fl == SPRITE_END) {
				return -1;
			}
			if (fl & SPRITE_SOLID_NOT) {
			} else if (fl & SPRITE_OFF) {
			} else if (!(fl & SPRITE_DRAWN)) {
			} else {
				uint32_t bx = o->x;
				uint32_t cxa = cx;
				if (dir < 2) {
					cxa = (cx + s->width - 1) & 0xffff;
				}
				if (bx > cxa) {
					goto next;
				}
				bx = (bx + o->width - 1) & 0xffff;
				/* bx + width - 1 >= cx */
				if (bx < cx) {
					goto next;
				}
				bx = o->y;
				uint32_t dxa = dx;
				if (dir >= 2) {
					dxa = (dx + s->height - 1) & 0xffff;
				}
				if (bx > dxa) {
					goto next;
				}
				bx = (o->y + o->height - 1) & 0xffff;
				if (bx >= dx) {
					return (int)ax;
				}
			}
		}
next:
		o++;
		ax++;
	}
}

/* Sub-op 9, 0x40500e: collision test for sprite n. AX = the index of the
 * sprite in the way, or 0xffff. */
uint32_t game_map_collision(uint32_t n)
{
	uint32_t n16 = n << 4;
	uint32_t s = (n16 & 0xffff0000u) | ((n16 + map_rec()->sprites) & 0xffff);
	return (uint32_t)game_map_collide(sprite_at(s)) & 0xffff;
}

/* ---- the dispatcher */

/* The sub-ops that run to their end without waiting; 5, 7 and 12 wait for
 * the window (map_pump.c); the table of the native dispatcher has nothing at
 * the other numbers. */
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

/* 0x403a70 up to its call of the sub-op: copy arguments 1..6 of the list to
 * G.map.v.args (the call of 0x403a70 and of the sub-op are counted here too) */
void game_map_load_args(void)
{
	memcpy(G.map.v.args, &G.rt.arg_value[1], 12);
}

/* 0x403a70 for the sub-ops that do not wait; returns the AX the sub-op leaves
 * (the sub-op number if it sets none) */
uint32_t game_map_op(uint32_t op)
{
	uint32_t eax = op;
	game_map_load_args();
	switch (op) {
	case 0:
		eax = op_init();
		break;
	case 1:
		op_player_script();
		break;
	case 2:
		eax = game_map_fill_grid(eax);
		break;
	case 3:
		eax = game_map_overlay_sprites(eax);
		break;
	case 4:
		eax = game_map_redraw_changed();
		break;
	case 6: /* 0x4042e7 */
		eax = game_map_fill_grid(eax);
		eax = game_map_overlay_sprites(eax);
		eax = game_map_redraw_changed();
		break;
	case 8:
		eax = game_map_area_trigger(MV(args[0]));
		break;
	case 9:
		eax = game_map_collision(MV(args[0]));
		break;
	case 10:
		eax = op_enter();
		break;
	case 11:
		eax = op_reset_history();
		break;
	case 13:
		eax = op_sprite_script();
		break;
	case 14:
		eax = op_sprite_enable();
		break;
	case 15:
		eax = op_unpack();
		break;
	case 16:
		eax = op_copy_cells();
		break;
	case 30:
		eax = game_map_op_walk();
		break;
	case 32:
		op_stop_walk();
		break;
	default:
		game_trap(0x403a7f, "map sub-op left to the native code reached C");
	}
	return eax;
}
