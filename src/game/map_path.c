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

#define PATH_GRID 0x435e62u      /* u16: copy of the walk grid's offset (written, never read) */
#define PATH_FROM_CELL 0x435e64u /* u16: the sprite's cell in the grid */
#define PATH_GOAL_CELL 0x435e66u /* u16: the goal's cell */
#define PATH_GOAL_X 0x435e68u    /* u16 */
#define PATH_GOAL_Y 0x435e6au
#define PATH_FROM_X 0x435e6cu    /* u16: the sprite's cell */
#define PATH_FROM_Y 0x435e6eu
#define PATH_W 0x435e70u         /* u16: the map's size */
#define PATH_H 0x435e72u
#define APPROACH 0x435e61u       /* u8: bit 0, walk up to a goal that is a wall */

#define GRID_WALL 0xffu
#define GRID_FREE 0xfeu
#define GRID_GOAL 0xfdu

static uint32_t grid_at(uint32_t i)
{
	return rd8(SEG1 + i);
}

/* 0x40562b: the grid from the layer. Cell (x, y) is blocked if the layer
 * tile at (x, y + 1) or (x + 1, y + 1) has its wall bit (bit 15). */
static void build_walk_grid(uint32_t m)
{
	uint32_t grid = rd16(MAP_WALK_BASE);
	uint32_t tile = rd16(m + M_LAYER) + 4;
	tile = (tile & 0xffff0000u) | ((tile + rd16(m + M_MAP_W)) & 0xffff);
	tile = (tile & 0xffff0000u) | ((tile + rd16(m + M_MAP_W)) & 0xffff);
	uint32_t w = rd16(m + M_MAP_W), h = rd16(m + M_MAP_H);
	X86_COUNT(1 + 12 + 2); /* the call, up to the first push edi, pop esi + ret */
	for (uint32_t r = h; r; r--) {
		X86_COUNT(1); /* movzx ecx */
		for (uint32_t c = w; c; c--) {
			uint32_t v = GRID_WALL;
			if (rd8(SEG1 + tile + 1) & 0x80) {
				X86_COUNT(11);
			} else if (rd8(SEG1 + tile + 3) & 0x80) {
				X86_COUNT(13);
			} else {
				v = GRID_FREE;
				X86_COUNT(14);
			}
			wr8(SEG1 + grid, v);
			tile += 2;
			grid++;
			X86_POLL(0x40568a);
		}
		X86_COUNT(2); /* pop ecx, loop */
		X86_POLL(0x40568d);
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
	X86_COUNT(10); /* the prologue and the first test */
	if (grid_at(cell) == GRID_WALL) {
		X86_COUNT(1);
		goto fail;
	}
	X86_COUNT(3);
	if (x < 0) {
		X86_COUNT(1);
		goto fail;
	}
	X86_COUNT(4);
	if ((int32_t)rd16(PATH_W) - 1 < x) {
		X86_COUNT(1);
		goto fail;
	}
	X86_COUNT(3);
	if (y < 0) {
		X86_COUNT(1);
		goto fail;
	}
	X86_COUNT(4);
	if ((int32_t)rd16(PATH_H) - 1 < y) {
		X86_COUNT(1);
		goto fail;
	}
	X86_COUNT(2);
	if (depth < 0) {
		X86_COUNT(1);
		goto fail;
	}
	X86_COUNT(4);
	if (grid_at(cell) == 0) {
		wr8(SEG1 + cell, (uint32_t)depth);
		X86_COUNT(1 + 8);
		return 1;
	}
	uint32_t px = rd16(PATH_FROM_X), py = rd16(PATH_FROM_Y);
	X86_COUNT(2);
	if ((mask & 0xffff) == 0xffff) {
		mask &= 0xffff0000u;
		X86_COUNT(1 + 3);
		if ((int16_t)((uint32_t)y - py) < 0) {
			mask |= 1;
			X86_COUNT(1);
		}
		X86_COUNT(3);
		if ((int16_t)((uint32_t)x - px) < 0) {
			mask |= 2;
			X86_COUNT(2);
		}
		goto axis;
	}
	X86_COUNT(3);
	if ((uint32_t)y != py) {
		mask = (mask & 0xffff0000u) | (mask & 6);
		X86_COUNT(4);
		if ((int16_t)((uint32_t)y - py) < 0) {
			mask |= 1;
			X86_COUNT(1);
		}
	}
	X86_COUNT(3);
	if ((uint32_t)x != px) {
		mask = (mask & 0xffff0000u) | (mask & 5);
		X86_COUNT(4);
		if ((int16_t)((uint32_t)x - px) < 0) {
			mask |= 2;
			X86_COUNT(1);
		}
	}
	{
		int32_t ay = abs16((int32_t)((uint32_t)y - py));
		int32_t ax = abs16((int32_t)((uint32_t)x - px));
		int32_t d = abs16(ax - ay);
		X86_COUNT(17);
		if (d <= 3)
			goto visit;
		mask = (mask & 0xffff0000u) | (mask & 3);
		X86_COUNT(1);
	}
axis:
	{
		int32_t ax = abs16((int32_t)((uint32_t)x - px));
		int32_t ay = abs16((int32_t)((uint32_t)y - py));
		X86_COUNT(13);
		if (!(ay >= ax)) {
			mask |= 4;
			X86_COUNT(1);
		}
	}
visit:
	wr8(SEG1 + cell, GRID_WALL);
	depth--;
	X86_COUNT(6);
	if ((mask & 0xffff) > 7)
		goto won;
	for (int k = 0; k < 4; k++) {
		int32_t nx = x, ny = y;
		uint32_t nc = cell;
		uint32_t w = rd16(PATH_W);
		switch (order[mask & 7][k]) {
		case U: ny = y - 1; nc = (cell & 0xffff0000u) | ((cell - w) & 0xffff); break;
		case D: ny = y + 1; nc = cell + w; break;
		case L: nx = x - 1; nc = cell - 1; break;
		default: nx = x + 1; nc = cell + 1; break;
		}
		X86_COUNT(12);
		if (flood(nx, ny, mask, nc, depth))
			goto won;
	}
fail:
	X86_COUNT(6);
	return 0;
won:
	wr8(SEG1 + cell, (uint32_t)(depth + 1));
	X86_COUNT(9);
	return 1;
}

/* 0x405acd: search from the goal; if there is no route the sprite's own
 * cell is blocked */
static void start_flood(void)
{
	int32_t goal_x = (int32_t)rd16(PATH_GOAL_X), goal_y = (int32_t)rd16(PATH_GOAL_Y);
	uint32_t goal = rd16(PATH_GOAL_CELL);
	X86_COUNT(1 + 1 + 8 + 2); /* the call, pushal, up to the call, or, jne */
	if (!flood(goal_x, goal_y, 0xffff, goal, (int32_t)GRID_GOAL)) {
		wr8(SEG1 + rd16(PATH_FROM_CELL), GRID_WALL);
		X86_COUNT(2);
	}
	X86_COUNT(2); /* popal, ret */
}

/* 0x405fb6: the goal is a wall (marked 0xfd, `level`): from the cells of
 * each level, the free neighbours (not across the sprite's footprint: below
 * only if row + height is inside the map, right only if column + width is)
 * get the next level down, until a pass adds none */
static void approach(uint32_t m, uint32_t s)
{
	uint32_t level = GRID_GOAL;
	X86_COUNT(1); /* the call */
	for (;;) {
		uint32_t progress = 0;
		uint32_t cell = rd16(MAP_WALK_BASE);
		X86_COUNT(5);
		uint32_t rows = 0;
		do {
			uint32_t cols = 0;
			X86_COUNT(1);
			do {
				X86_COUNT(2 + 4);
				if (grid_at(cell) == level) {
					uint32_t mw = rd16(m + M_MAP_W);
					uint32_t n;
					X86_COUNT(2);
					if (rows != 0) {
						n = (cell & 0xffff0000u) | ((cell - mw) & 0xffff);
						X86_COUNT(5);
						if (grid_at(n) == GRID_FREE) {
							wr8(SEG1 + n, level - 1);
							progress = 0xff;
							X86_COUNT(4);
						}
					}
					X86_COUNT(7);
					if (((rd8(SEG1 + s + S_HEIGHT) + rows) & 0xffff) < rd16(m + M_MAP_H)) {
						n = (cell & 0xffff0000u) | ((cell + rd16(m + M_MAP_W)) & 0xffff);
						X86_COUNT(5);
						if (grid_at(n) == GRID_FREE) {
							wr8(SEG1 + n, level - 1);
							progress = 0xff;
							X86_COUNT(4);
						}
					}
					X86_COUNT(2);
					if (cols != 0) {
						n = cell - 1;
						X86_COUNT(5);
						if (grid_at(n) == GRID_FREE) {
							wr8(SEG1 + n, level - 1);
							progress = 0xff;
							X86_COUNT(4);
						}
					}
					X86_COUNT(7);
					if (((rd8(SEG1 + s + S_WIDTH) + cols) & 0xffff) < rd16(m + M_MAP_W)) {
						n = cell + 1;
						X86_COUNT(5);
						if (grid_at(n) == GRID_FREE) {
							wr8(SEG1 + n, level - 1);
							progress = 0xff;
							X86_COUNT(4);
						}
					}
				}
				cell++;
				cols++;
				X86_POLL(0x40607a);
			} while ((cols & 0xffff) != rd16(m + M_MAP_W));
			rows++;
			X86_COUNT(3);
			X86_POLL(0x405fc3);
		} while ((rows & 0xffff) != rd16(m + M_MAP_H));
		X86_COUNT(2);
		if (progress == 0) {
			X86_COUNT(1);
			return;
		}
		level = (level - 1) & 0xff;
		X86_COUNT(2);
		if (level == 0) {
			X86_COUNT(1);
			return;
		}
		X86_POLL(0x405fb8);
	}
}

static uint32_t cell_of(uint32_t m, uint32_t x, uint32_t y)
{
	return (((rd16(m + M_MAP_W) * y) & 0xffff) + x + rd16(MAP_WALK_BASE)) & 0xffff;
}

/* 0x405966: plan a walk for sprite ESI (an offset in segment 1) to the cell
 * (ECX, EDX). A walk already planned is cancelled instead (the next call
 * plans). Sets MAP_AUTOWALK. */
void game_map_plan_walk(void)
{
	uint32_t s = cpu.r[ESI], gx = cpu.r[ECX] & 0xffff, gy = cpu.r[EDX] & 0xffff;
	uint32_t sp = SEG1 + s;
	X86_COUNT(2);
	if (rd8(MAP_AUTOWALK) != 0) {
		wr8(MAP_AUTOWALK, 0);
		X86_COUNT(2);
		return;
	}
	uint32_t m = SEG1 + SV(map_sel);
	wr16(PATH_FROM_X, rd16(sp + S_X));
	wr16(PATH_FROM_Y, rd16(sp + S_Y));
	wr16(PATH_GOAL_X, gx);
	wr16(PATH_GOAL_Y, gy);
	wr16(PATH_W, rd16(m + M_MAP_W));
	wr16(PATH_H, rd16(m + M_MAP_H));
	wr16(PATH_GRID, rd16(MAP_WALK_BASE));
	X86_COUNT(17);
	build_walk_grid(m);
	/* every `mul` leaves the high word of its product in DX, and DX is the
	 * goal's y when the approach below starts over (usually 0: the goal
	 * is looked for in row 0) */
	uint32_t prod = gy * rd16(m + M_MAP_W);
	uint32_t dx_reg = prod >> 16;
	uint32_t goal = ((prod & 0xffff) + gx + rd16(MAP_WALK_BASE)) & 0xffff;
	X86_COUNT(6);
	if (grid_at(goal) != GRID_WALL) {
		wr16(PATH_GOAL_CELL, goal);
		uint32_t from = cell_of(m, rd16(sp + S_X), rd16(sp + S_Y));
		dx_reg = (rd16(m + M_MAP_W) * rd16(sp + S_Y)) >> 16;
		wr16(PATH_FROM_CELL, from);
		wr8(SEG1 + from, 0);
		X86_COUNT(9);
		start_flood();
		X86_COUNT(3);
		if (grid_at(goal) < GRID_FREE) {
			wr8(MAP_AUTOWALK, 0xff);
			X86_COUNT(2);
			return;
		}
	}
	X86_COUNT(2);
	if (!(rd8(APPROACH) & 1)) {
		wr8(MAP_AUTOWALK, 0);
		X86_COUNT(2);
		return;
	}
	X86_COUNT(5);
	build_walk_grid(m);
	uint32_t cell = ((((dx_reg * rd16(m + M_MAP_W)) & 0xffff) + gx + rd16(MAP_WALK_BASE)) & 0xffff);
	uint32_t old = grid_at(cell);
	wr8(SEG1 + cell, GRID_GOAL);
	X86_COUNT(10);
	approach(m, s);
	X86_COUNT(4);
	if (old == GRID_WALL) {
		wr8(SEG1 + cell, GRID_WALL);
		X86_COUNT(1);
	}
	uint32_t at = cell_of(m, rd16(sp + S_X), rd16(sp + S_Y));
	uint32_t v = grid_at(at);
	X86_COUNT(7 + 2 + (v == GRID_WALL ? 2 : v == GRID_FREE ? 4 : 4));
	wr8(MAP_AUTOWALK, (v == GRID_WALL || v == GRID_FREE) ? 0 : 0xff);
}

/* 0x405691: one step of the walk: from the sprite's cell (mark v) to a
 * neighbour with a higher mark below 0xfe. Eight directions, tried
 * diagonals first; a diagonal is made of two orthogonal steps, in the
 * order of the neighbour that is itself marked. No step possible (or the
 * step is refused): the walk ends. EDI = the map selector on return. */

/* the step(s): one mover, or two (AL = both results or-ed); 0 = refused */
static int walk(uint32_t sel, void (*a)(void), void (*b)(void))
{
	cpu.r[EDI] = sel;
	X86_COUNT(2); /* mov edi, call */
	a();
	uint32_t al = cpu.r[EAX] & 0xff;
	if (b) {
		X86_COUNT(5); /* push, call, pop, or, cmp */
		b();
		al = (cpu.r[EAX] | al) & 0xff;
	} else {
		X86_COUNT(1); /* cmp */
	}
	X86_COUNT(2); /* je (+ ret) */
	if (al == 0)
		X86_COUNT(3); /* the stop: mov edi, mov byte, ret */
	return al != 0;
}

/* one diagonal neighbour n of the cell: 3 instructions to find it, 2 + 2 to
 * test it (below 0xfe, and a higher mark than v); if usable the two
 * orthogonal neighbours o1 (first) and o2 say in which order to step */
struct diag {
	uint32_t n, o1, o2;
	void (*first1)(void), (*second1)(void), (*first2)(void), (*second2)(void);
	int tail; /* instructions after both tests fail */
};

static int diagonal(uint32_t sel, uint32_t v, const struct diag *d, int *done)
{
	X86_COUNT(3 + 2);
	if (grid_at(d->n) >= GRID_FREE)
		return 0;
	X86_COUNT(2);
	if (!(v < grid_at(d->n)))
		return 0;
	X86_COUNT(3);
	if (grid_at(d->o1) < GRID_FREE) {
		*done = walk(sel, d->first1, d->second1);
		return 1;
	}
	X86_COUNT(4);
	if (grid_at(d->o2) < GRID_FREE) {
		*done = walk(sel, d->first2, d->second2);
		return 1;
	}
	X86_COUNT(d->tail);
	return 0;
}

/* an orthogonal neighbour: `setup` instructions to find it, then the test */
static int orthogonal(uint32_t v, uint32_t n, int setup)
{
	X86_COUNT(setup + 2);
	if (grid_at(n) >= GRID_FREE)
		return 0;
	X86_COUNT(2);
	return v < grid_at(n);
}

void game_map_walk_step(void)
{
	uint32_t s = cpu.r[ESI];
	uint32_t sp = SEG1 + s;
	uint32_t sel = SV(map_sel);
	uint32_t m = SEG1 + sel;
	uint32_t mw = rd16(m + M_MAP_W);
	uint32_t c = (((mw * rd16(sp + S_Y)) & 0xffff) + rd16(sp + S_X) + rd16(MAP_WALK_BASE)) & 0xffff;
	uint32_t v = grid_at(c);
	X86_COUNT(10);
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
		if (diagonal(sel, v, &ul, &done) || diagonal(sel, v, &ur, &done) ||
		    diagonal(sel, v, &dr, &done) || diagonal(sel, v, &dl, &done)) {
			if (done)
				return;
		} else if (orthogonal(v, c - 1, 2)) {
			if (walk(sel, game_map_move_left, NULL))
				return;
		} else if (orthogonal(v, c + 1, 1)) {
			if (walk(sel, game_map_move_right, NULL))
				return;
		} else if (orthogonal(v, c + mw, 2)) {
			if (walk(sel, game_map_move_down, NULL))
				return;
		} else if (orthogonal(v, c - mw, 2)) {
			if (walk(sel, game_map_move_up, NULL))
				return;
		} else {
			X86_COUNT(3); /* the stop */
		}
	} else {
		X86_COUNT(3);
	}
	cpu.r[EDI] = sel;
	wr8(MAP_AUTOWALK, 0);
}

/* Sub-op 30, 0x405542: plan a walk for sprite a to the destination in its
 * record (S_DEST_X, S_DEST_Y), approaching a blocked goal. S = 0 if a walk
 * was planned, 0xffff - 0x0101 * flag... as the native code computes it:
 * -(flag * 0x101 + 1). */
uint32_t game_map_op_walk(void)
{
	uint32_t sel = SV(map_sel);
	uint32_t n16 = rd16(MAP_ARGS) << 4;
	uint32_t s = ((n16) + rd16(SEG1 + sel + M_SPRITES)) & 0xffff;
	uint32_t sp = SEG1 + s;
	cpu.r[ESI] = s;
	cpu.r[ECX] = rd8(sp + S_DEST_X);
	cpu.r[EDX] = rd8(sp + S_DEST_Y);
	wr8(APPROACH, 1);
	X86_COUNT(14);
	game_map_plan_walk();
	uint32_t f = rd8(MAP_AUTOWALK);
	uint32_t bx = (f * 0x101 + 1) & 0xffff;
	X86_COUNT(6);
	return (0u - bx) & 0xffff;
}
