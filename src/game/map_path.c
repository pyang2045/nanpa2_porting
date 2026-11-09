/*
 * The town map, part 4 (stage 5A): walking to a place.
 *
 * The walk grid (WALK_BASE: one byte per map cell, after the layer) turns
 * the map into a route for one sprite. build_walk_grid marks every cell 0xff
 * (blocked: the layer's tile in the row below, or the next one, has the
 * wall bit) or 0xfe (free); the sprite's own cell is cleared to 0, and a
 * depth-first search that starts at the goal with 0xfd and counts down
 * (flood) writes, on the way back along the first route it finds to the
 * sprite, each cell's remaining distance; cells it gave up on stay 0xff.
 * Walking (walk_step) is then a climb: step to a neighbour with a higher
 * mark. If the goal is a wall, or the search fails, and the caller asked for
 * it (APPROACH bit 0), a pass of approach marks the free cells around the
 * goal outwards so that the sprite walks up to it. The result is
 * MAP_AUTOWALK = 0xff (a route exists) or 0.
 *
 * The search tries the four neighbours in an order picked from the
 * direction of the sprite (a mask of three bits: up/down, left/right and
 * which axis is longer), which is why the routes it finds look natural. The
 * C keeps the exact orders, and the native recursion's reads and writes.
 *
 * Native: 0x405966 plan, 0x40562b grid, 0x405acd + 0x405b07 + eight cases
 * flood, 0x405fb6 approach, 0x405691 step, 0x405542 sub-op 30.
 */
#include "game/map.h"
#include "game/state.h"


#define GRID_WALL 0xffu
#define GRID_FREE 0xfeu
#define GRID_GOAL 0xfdu

static uint32_t grid_at(uint32_t i)
{
	return seg1_u8(i);
}

/* 0x40562b: the grid from the layer. Cell (x, y) is blocked if the layer
 * tile at (x, y + 1) or (x + 1, y + 1) has its wall bit (bit 15). */
static void build_walk_grid(const struct map_rec *m)
{
	uint32_t grid = MV(walk_base);
	uint32_t tile = m->layer + 4;
	tile = (tile & 0xffff0000u) | ((tile + m->map_w) & 0xffff);
	tile = (tile & 0xffff0000u) | ((tile + m->map_w) & 0xffff);
	uint32_t w = m->map_w, h = m->map_h;
	for (uint32_t r = h; r; r--) {
		for (uint32_t c = w; c; c--) {
			uint32_t v = GRID_WALL;
			if (seg1_u8(tile + 1) & 0x80) {
			} else if (seg1_u8(tile + 3) & 0x80) {
			} else {
				v = GRID_FREE;
			}
			seg1_set8(grid, v);
			tile += 2;
			grid++;
		}
	}
}

/* the order the search tries the neighbours in, by mask 0..7 (U up, D down,
 * L left, R right) */
enum { U, D, L, R };
static const uint8_t order[8][4] = {
	{ U, L, R, D }, { D, L, R, U }, { U, R, L, D }, { D, R, L, U },
	{ L, U, D, R }, { L, D, U, R }, { R, U, D, L }, { R, D, U, L },
};

static int32_t abs16(int32_t v)
{
	int16_t s = (int16_t)v;
	return s < 0 ? (int16_t)-s : s;
}

/* 0x405b07: search from (x, y) at grid cell `cell` with `depth`; 1 if a
 * route to the sprite (the cell holding 0) was found, with the cells on it
 * marked by their depth */
static int flood(int32_t x, int32_t y, uint32_t mask, uint32_t cell, int32_t depth)
{
	if (grid_at(cell) == GRID_WALL) {
		goto fail;
	}
	if (x < 0) {
		goto fail;
	}
	if ((int32_t)MV(path_w) - 1 < x) {
		goto fail;
	}
	if (y < 0) {
		goto fail;
	}
	if ((int32_t)MV(path_h) - 1 < y) {
		goto fail;
	}
	if (depth < 0) {
		goto fail;
	}
	if (grid_at(cell) == 0) {
		seg1_set8(cell, (uint32_t)depth);
		return 1;
	}
	uint32_t px = MV(path_from_x), py = MV(path_from_y);
	if ((mask & 0xffff) == 0xffff) {
		mask &= 0xffff0000u;
		if ((int16_t)((uint32_t)y - py) < 0) {
			mask |= 1;
		}
		if ((int16_t)((uint32_t)x - px) < 0) {
			mask |= 2;
		}
		goto axis;
	}
	if ((uint32_t)y != py) {
		mask = (mask & 0xffff0000u) | (mask & 6);
		if ((int16_t)((uint32_t)y - py) < 0) {
			mask |= 1;
		}
	}
	if ((uint32_t)x != px) {
		mask = (mask & 0xffff0000u) | (mask & 5);
		if ((int16_t)((uint32_t)x - px) < 0) {
			mask |= 2;
		}
	}
	{
		int32_t ay = abs16((int32_t)((uint32_t)y - py));
		int32_t ax = abs16((int32_t)((uint32_t)x - px));
		int32_t d = abs16(ax - ay);
		if (d <= 3)
			goto visit;
		mask = (mask & 0xffff0000u) | (mask & 3);
	}
axis:
	{
		int32_t ax = abs16((int32_t)((uint32_t)x - px));
		int32_t ay = abs16((int32_t)((uint32_t)y - py));
		if (!(ay >= ax)) {
			mask |= 4;
		}
	}
visit:
	seg1_set8(cell, GRID_WALL);
	depth--;
	if ((mask & 0xffff) > 7)
		goto won;
	for (int k = 0; k < 4; k++) {
		int32_t nx = x, ny = y;
		uint32_t nc = cell;
		uint32_t w = MV(path_w);
		switch (order[mask & 7][k]) {
		case U: ny = y - 1; nc = (cell & 0xffff0000u) | ((cell - w) & 0xffff); break;
		case D: ny = y + 1; nc = cell + w; break;
		case L: nx = x - 1; nc = cell - 1; break;
		default: nx = x + 1; nc = cell + 1; break;
		}
		if (flood(nx, ny, mask, nc, depth))
			goto won;
	}
fail:
	return 0;
won:
	seg1_set8(cell, (uint32_t)(depth + 1));
	return 1;
}

/* 0x405acd: search from the goal; if there is no route the sprite's own
 * cell is blocked */
static void start_flood(void)
{
	int32_t goal_x = (int32_t)MV(path_goal_x), goal_y = (int32_t)MV(path_goal_y);
	uint32_t goal = MV(path_goal_cell);
	if (!flood(goal_x, goal_y, 0xffff, goal, (int32_t)GRID_GOAL)) {
		seg1_set8(MV(path_from_cell), GRID_WALL);
	}
}

/* 0x405fb6: the goal is a wall (marked 0xfd, `level`): from the cells of
 * each level, the free neighbours (not across the sprite's footprint: below
 * only if row + height is inside the map, right only if column + width is)
 * get the next level down, until a pass adds none */
static void approach(const struct map_rec *m, const struct sprite *s)
{
	uint32_t level = GRID_GOAL;
	for (;;) {
		uint32_t progress = 0;
		uint32_t cell = MV(walk_base);
		uint32_t rows = 0;
		do {
			uint32_t cols = 0;
			do {
				if (grid_at(cell) == level) {
					uint32_t mw = m->map_w;
					uint32_t n;
					if (rows != 0) {
						n = (cell & 0xffff0000u) | ((cell - mw) & 0xffff);
						if (grid_at(n) == GRID_FREE) {
							seg1_set8(n, level - 1);
							progress = 0xff;
						}
					}
					if (((s->height + rows) & 0xffff) < m->map_h) {
						n = (cell & 0xffff0000u) | ((cell + m->map_w) & 0xffff);
						if (grid_at(n) == GRID_FREE) {
							seg1_set8(n, level - 1);
							progress = 0xff;
						}
					}
					if (cols != 0) {
						n = cell - 1;
						if (grid_at(n) == GRID_FREE) {
							seg1_set8(n, level - 1);
							progress = 0xff;
						}
					}
					if (((s->width + cols) & 0xffff) < m->map_w) {
						n = cell + 1;
						if (grid_at(n) == GRID_FREE) {
							seg1_set8(n, level - 1);
							progress = 0xff;
						}
					}
				}
				cell++;
				cols++;
			} while ((cols & 0xffff) != m->map_w);
			rows++;
		} while ((rows & 0xffff) != m->map_h);
		if (progress == 0) {
			return;
		}
		level = (level - 1) & 0xff;
		if (level == 0) {
			return;
		}
	}
}

static uint32_t cell_of(const struct map_rec *m, uint32_t x, uint32_t y)
{
	return (((m->map_w * y) & 0xffff) + x + MV(walk_base)) & 0xffff;
}

/* 0x405966: plan a walk for sprite s to the cell (gx, gy), 16 bits each. A
 * walk already planned is cancelled instead (the next call plans). Sets
 * G.map.v.autowalk. */
void game_map_plan_walk(struct sprite *s, uint32_t gx, uint32_t gy)
{
	gx &= 0xffff;
	gy &= 0xffff;
	if (MV(autowalk) != 0) {
		MV_SET(autowalk, 0);
		return;
	}
	const struct map_rec *m = map_rec();
	MV_SET(path_from_x, s->x);
	MV_SET(path_from_y, s->y);
	MV_SET(path_goal_x, gx);
	MV_SET(path_goal_y, gy);
	MV_SET(path_w, m->map_w);
	MV_SET(path_h, m->map_h);
	MV_SET(path_grid, MV(walk_base));
	build_walk_grid(m);
	/* every `mul` leaves the high word of its product in DX, and DX is the
	 * goal's y when the approach below starts over (usually 0: the goal
	 * is looked for in row 0) */
	uint32_t prod = gy * m->map_w;
	uint32_t dx_reg = prod >> 16;
	uint32_t goal = ((prod & 0xffff) + gx + MV(walk_base)) & 0xffff;
	if (grid_at(goal) != GRID_WALL) {
		MV_SET(path_goal_cell, goal);
		uint32_t from = cell_of(m, s->x, s->y);
		dx_reg = (m->map_w * s->y) >> 16;
		MV_SET(path_from_cell, from);
		seg1_set8(from, 0);
		start_flood();
		if (grid_at(goal) < GRID_FREE) {
			MV_SET(autowalk, 0xff);
			return;
		}
	}
	if (!(MV(approach) & 1)) {
		MV_SET(autowalk, 0);
		return;
	}
	build_walk_grid(m);
	uint32_t cell = ((((dx_reg * m->map_w) & 0xffff) + gx + MV(walk_base)) & 0xffff);
	uint32_t old = grid_at(cell);
	seg1_set8(cell, GRID_GOAL);
	approach(m, s);
	if (old == GRID_WALL) {
		seg1_set8(cell, GRID_WALL);
	}
	uint32_t at = cell_of(m, s->x, s->y);
	uint32_t v = grid_at(at);
	MV_SET(autowalk, (v == GRID_WALL || v == GRID_FREE) ? 0 : 0xff);
}

/* 0x405691: one step of the walk: from the sprite's cell (mark v) to a
 * neighbour with a higher mark below 0xfe. Eight directions, tried
 * diagonals first; a diagonal is made of two orthogonal steps, in the
 * order of the neighbour that is itself marked. No step possible (or the
 * step is refused): the walk ends. EDI = the map selector on return. */

typedef int (*mover_fn)(struct sprite *);

/* what the walk's steps need: the sprite, and the AX leftover that the
 * movers' results replace (sub-op 7 reports it as S) */
struct walker {
	struct sprite *s;
	uint32_t *eax;
};

/* the step(s): one mover, or two (the results or-ed); 0 = refused */
static int walk(const struct walker *w, mover_fn a, mover_fn b)
{
	int al = a(w->s);
	*w->eax = (*w->eax & 0xffffff00u) | (uint32_t)al;
	if (b) {
		int bl = b(w->s);
		*w->eax = (*w->eax & 0xffffff00u) | (uint32_t)bl;
		al = (al | bl) & 0xff;
	} else {
	}
	return al != 0;
}

/* one diagonal neighbour n of the cell: 3 instructions to find it, 2 + 2 to
 * test it (below 0xfe, and a higher mark than v); if usable the two
 * orthogonal neighbours o1 (first) and o2 say in which order to step */
struct diag {
	uint32_t n, o1, o2;
	mover_fn first1, second1, first2, second2;
	int tail; /* instructions after both tests fail */
};

static int diagonal(const struct walker *w, uint32_t v, const struct diag *d, int *done)
{
	if (grid_at(d->n) >= GRID_FREE)
		return 0;
	if (!(v < grid_at(d->n)))
		return 0;
	if (grid_at(d->o1) < GRID_FREE) {
		*done = walk(w, d->first1, d->second1);
		return 1;
	}
	if (grid_at(d->o2) < GRID_FREE) {
		*done = walk(w, d->first2, d->second2);
		return 1;
	}
	return 0;
}

/* an orthogonal neighbour: `setup` instructions to find it, then the test */
static int orthogonal(uint32_t v, uint32_t n, int setup)
{
	if (grid_at(n) >= GRID_FREE)
		return 0;
	return v < grid_at(n);
}

void game_map_walk_step(struct sprite *s, uint32_t *eax)
{
	const struct map_rec *m = map_rec();
	struct walker wk = { s, eax };
	const struct walker *w = &wk;
	uint32_t mw = m->map_w;
	uint32_t c = (((mw * s->y) & 0xffff) + s->x + MV(walk_base)) & 0xffff;
	uint32_t v = grid_at(c);
	int done = 0;
	if (v < GRID_GOAL) {
		/* up-left, up-right, down-right, down-left (an orthogonal's two steps first, then the other order) */
		struct diag ul = { (c - mw - 1) & 0xffff, (c - mw) & 0xffff, (c - 1) & 0xffff,
			game_map_move_up, game_map_move_left, game_map_move_left, game_map_move_up, 0 };
		struct diag ur = { (c - mw + 1) & 0xffff, (c - mw) & 0xffff, (c + 1) & 0xffff,
			game_map_move_up, game_map_move_right, game_map_move_right, game_map_move_up, 1 };
		struct diag dr = { (c + mw + 1) & 0xffff, (c + mw) & 0xffff, (c + 1) & 0xffff,
			game_map_move_down, game_map_move_right, game_map_move_right, game_map_move_down, 1 };
		struct diag dl = { (c + mw - 1) & 0xffff, (c + mw) & 0xffff, (c - 1) & 0xffff,
			game_map_move_down, game_map_move_left, game_map_move_left, game_map_move_down, 1 };
		if (diagonal(w, v, &ul, &done) || diagonal(w, v, &ur, &done) ||
		    diagonal(w, v, &dr, &done) || diagonal(w, v, &dl, &done)) {
			if (done)
				return;
		} else if (orthogonal(v, c - 1, 2)) {
			if (walk(w, game_map_move_left, NULL))
				return;
		} else if (orthogonal(v, c + 1, 1)) {
			if (walk(w, game_map_move_right, NULL))
				return;
		} else if (orthogonal(v, c + mw, 2)) {
			if (walk(w, game_map_move_down, NULL))
				return;
		} else if (orthogonal(v, c - mw, 2)) {
			if (walk(w, game_map_move_up, NULL))
				return;
		} else {
		}
	} else {
	}
	MV_SET(autowalk, 0);
}

/* Sub-op 30, 0x405542: plan a walk for sprite a to the destination in its
 * record (dest_x, dest_y), approaching a blocked goal. AX = 0 if a walk
 * was planned, 0xffff - 0x0101 * flag... as the native code computes it:
 * -(flag * 0x101 + 1). */
uint32_t game_map_op_walk(void)
{
	struct sprite *s = sprite_at(((MV(args[0]) << 4) + map_rec()->sprites) & 0xffff);
	MV_SET(approach, 1);
	game_map_plan_walk(s, s->dest_x, s->dest_y);
	uint32_t f = MV(autowalk);
	uint32_t bx = (f * 0x101 + 1) & 0xffff;
	return (0u - bx) & 0xffff;
}
