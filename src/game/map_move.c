/*
 * The town map, part 3 (stage 5A): the four movers 0x404731 (up), 0x404937
 * (down), 0x404a9a (left) and 0x404c9f (right).
 *
 * A mover turns the sprite (ESI, an offset in segment 1; EDI = the map
 * selector) to face its direction, then tries to step one cell: it fails
 * (AL = 0) if another sprite is in the way (collide, map.c) or a wall is;
 * otherwise the sprite's cell changes and, for a sprite the view follows
 * (SPRITE_SCROLL), the view scrolls if the sprite came within the margin
 * of the view's edge (AL = 0xff).
 *
 * A wall is bit 15 of the layer's tile word. Only the sprite's feet row (the
 * bottom row of its footprint) counts, across its whole width when moving
 * up or down and at the one cell ahead when moving sideways. When the step
 * is blocked by a wall (or the edge of the map) the sprite slides: it tries
 * a step to the side that is open, and a blocked sideways step slides up
 * or down the same way. The native code falls from one mover into the tail
 * of another (up's slide goes to left's tail, and so on); here each tail
 * and each "face and try" is a function of its own.
 *
 * Quirk of the native code kept: the test for the map's right edge (0x404cb8
 * and, in the slide to the right, 0x4048d7) adds the sprite's height, not its
 * width, to x; everything else on the right uses the width.
 *
 * The movers' result is AL; EAX otherwise and ECX, EDX and EBX are
 * scratch (the callers read only AL, which the poisoned diff runs check);
 * ESI, EDI and EBP are kept.
 */
#include "game/map.h"

int game_map_collide(uint32_t s);

struct mv {
	uint32_t s;  /* the sprite's offset in segment 1 (ESI) */
	uint32_t sp; /* its address */
	uint32_t m;  /* the map state record's address (SEG1 + EDI) */
};

/* 0x404e06: the address (offset in segment 1) of the layer's tile word at
 * (x, y), 16-bit arithmetic */
static uint32_t cell_addr(const struct mv *c, uint32_t x, uint32_t y)
{
	uint32_t w2 = (rd16(c->m + M_MAP_W) * 2) & 0xffff;
	X86_COUNT(9); /* call, movzx, add, mul, add, add, add, add, ret */
	return (((w2 * (y & 0xffff)) & 0xffff) + x + x + rd16(c->m + M_LAYER) + 4) & 0xffff;
}

/* the wall bit of the tile word at offset a: its high byte's top bit */
static int wall(uint32_t a)
{
	return (rd8(SEG1 + a + 1) & 0x80) != 0;
}

/* the sprite's frame: its step bit, then the direction * 2 (up 0, down 2,
 * left 4, right 6) */
static void face(const struct mv *c, uint32_t dir2)
{
	wr8(c->sp + S_FRAME, (rd8(c->sp + S_FRAME) & 1) + dir2);
	X86_COUNT(dir2 ? 4 : 3);
}

static int blocked(const struct mv *c)
{
	X86_COUNT(2); /* call, jne */
	int r = game_map_collide(c->s) != -1;
	if (r)
		X86_COUNT(2); /* mov al, 0; ret */
	return r;
}

static uint32_t foot_row(const struct mv *c, int adjust)
{
	return (rd16(c->sp + S_Y) + rd8(c->sp + S_HEIGHT) + adjust) & 0xffff;
}

/* ---- the tails: step, then scroll the view if the sprite is near its edge */

/* the scroll test: with d the distance from the margin, `far` means the
 * view stays, else it moves unless it is at its limit (`at_limit`) */
static int tail_end(int scrolled, int at_limit)
{
	/* mov al, -1; ret, and the dec + jmp of a scroll */
	X86_COUNT(scrolled && !at_limit ? 4 : 2);
	return 0xff;
}

static int up_tail(const struct mv *c)
{
	uint32_t y = (rd16(c->sp + S_Y) - 1) & 0xffff;
	wr16(c->sp + S_Y, y);
	X86_COUNT(3);
	if (!(rd8(c->sp + S_FLAGS) & SPRITE_SCROLL))
		return tail_end(0, 0);
	uint32_t vy = rd16(c->m + M_VIEW_Y);
	uint32_t d = ((rd16(c->sp + S_Y) - vy + 1) & 0xffff);
	uint32_t margin = rd16(c->m + M_MARGIN_T);
	X86_COUNT(d == margin ? 7 : 8);
	if (d > margin)
		return tail_end(0, 0);
	vy = rd16(c->m + M_VIEW_Y);
	X86_COUNT(3);
	if (vy == rd16(c->m + M_MIN_Y))
		return tail_end(0, 0);
	wr16(c->m + M_VIEW_Y, (vy - 1) & 0xffff);
	return tail_end(1, 0);
}

static int down_tail(const struct mv *c)
{
	uint32_t y = (rd16(c->sp + S_Y) + 1) & 0xffff;
	wr16(c->sp + S_Y, y);
	X86_COUNT(3);
	if (!(rd8(c->sp + S_FLAGS) & SPRITE_SCROLL))
		return tail_end(0, 0);
	uint32_t vy = rd16(c->m + M_VIEW_Y);
	uint32_t d = (rd16(c->sp + S_Y) + rd8(c->sp + S_HEIGHT) - vy + rd16(c->m + M_MARGIN_B) - 1) & 0xffff;
	X86_COUNT(12);
	if (d < rd16(c->m + M_VIEW_H))
		return tail_end(0, 0);
	vy = (rd16(c->m + M_VIEW_Y) + rd16(c->m + M_VIEW_H) - 1) & 0xffff;
	X86_COUNT(5);
	if (vy == rd16(c->m + M_MAX_Y))
		return tail_end(0, 0);
	wr16(c->m + M_VIEW_Y, (rd16(c->m + M_VIEW_Y) + 1) & 0xffff);
	return tail_end(1, 0);
}

static int left_tail(const struct mv *c)
{
	uint32_t x = (rd16(c->sp + S_X) - 1) & 0xffff;
	wr16(c->sp + S_X, x);
	X86_COUNT(3);
	if (!(rd8(c->sp + S_FLAGS) & SPRITE_SCROLL))
		return tail_end(0, 0);
	uint32_t vx = rd16(c->m + M_VIEW_X);
	uint32_t d = ((rd16(c->sp + S_X) - vx + 1) & 0xffff);
	uint32_t margin = rd16(c->m + M_MARGIN_L);
	X86_COUNT(d == margin ? 7 : 8);
	if (d > margin)
		return tail_end(0, 0);
	vx = rd16(c->m + M_VIEW_X);
	X86_COUNT(3);
	if (vx == rd16(c->m + M_MIN_X))
		return tail_end(0, 0);
	wr16(c->m + M_VIEW_X, (vx - 1) & 0xffff);
	return tail_end(1, 0);
}

static int right_tail(const struct mv *c)
{
	uint32_t x = (rd16(c->sp + S_X) + 1) & 0xffff;
	wr16(c->sp + S_X, x);
	X86_COUNT(3);
	if (!(rd8(c->sp + S_FLAGS) & SPRITE_SCROLL))
		return tail_end(0, 0);
	uint32_t vx = rd16(c->m + M_VIEW_X);
	uint32_t d = (rd16(c->sp + S_X) + rd8(c->sp + S_WIDTH) - vx + rd16(c->m + M_MARGIN_R) - 1) & 0xffff;
	X86_COUNT(12);
	if (d < rd16(c->m + M_VIEW_W))
		return tail_end(0, 0);
	vx = (rd16(c->m + M_VIEW_X) + rd16(c->m + M_VIEW_W) - 1) & 0xffff;
	X86_COUNT(5);
	if (vx == rd16(c->m + M_MAX_X))
		return tail_end(0, 0);
	wr16(c->m + M_VIEW_X, (rd16(c->m + M_VIEW_X) + 1) & 0xffff);
	return tail_end(1, 0);
}

/* ---- turning to a side (or up or down) and trying the step there */

static int left_face(const struct mv *c)
{
	face(c, 4);
	X86_COUNT(3);
	if (rd16(c->sp + S_X) == 0) {
		X86_COUNT(2);
		return 0;
	}
	if (blocked(c))
		return 0;
	uint32_t a = cell_addr(c, (rd16(c->sp + S_X) - 1) & 0xffff, foot_row(c, -1));
	X86_COUNT(7 + 3);
	if (wall(a)) {
		X86_COUNT(2);
		return 0;
	}
	X86_COUNT(1);
	return left_tail(c);
}

static int right_face(const struct mv *c)
{
	face(c, 6);
	X86_COUNT(6);
	/* the native test adds the sprite's height, not its width (0x4048d7) */
	if (((rd16(c->sp + S_X) + rd8(c->sp + S_HEIGHT)) & 0xffff) == rd16(c->m + M_MAP_W)) {
		X86_COUNT(2);
		return 0;
	}
	if (blocked(c))
		return 0;
	uint32_t x = (rd16(c->sp + S_X) + rd8(c->sp + S_WIDTH)) & 0xffff;
	uint32_t a = cell_addr(c, x, foot_row(c, -1));
	X86_COUNT(9 + 3);
	if (wall(a)) {
		X86_COUNT(2);
		return 0;
	}
	X86_COUNT(1);
	return right_tail(c);
}

/* up, entered from a sideways slide: step up if the footprint's feet row
 * is free there */
static int up_face(const struct mv *c)
{
	wr8(c->sp + S_FRAME, rd8(c->sp + S_FRAME) & 1);
	X86_COUNT(3 + 3);
	if (rd16(c->sp + S_Y) == 0) {
		X86_COUNT(2);
		return 0;
	}
	if (blocked(c))
		return 0;
	uint32_t a = cell_addr(c, rd16(c->sp + S_X), foot_row(c, -2));
	X86_COUNT(6 + 2);
	for (uint32_t n = rd8(c->sp + S_WIDTH); n; n--) {
		X86_COUNT(5);
		if (wall(a)) {
			X86_COUNT(2);
			return 0;
		}
		a += 2;
	}
	X86_COUNT(1);
	return up_tail(c);
}

static int down_face(const struct mv *c)
{
	face(c, 2);
	X86_COUNT(6);
	if (((rd16(c->sp + S_Y) + rd8(c->sp + S_HEIGHT)) & 0xffff) == rd16(c->m + M_MAP_H)) {
		X86_COUNT(2);
		return 0;
	}
	if (blocked(c))
		return 0;
	uint32_t a = cell_addr(c, rd16(c->sp + S_X), foot_row(c, 0));
	X86_COUNT(5 + 2);
	for (uint32_t n = rd8(c->sp + S_WIDTH); n; n--) {
		X86_COUNT(5);
		if (wall(a)) {
			X86_COUNT(2);
			return 0;
		}
		a += 2;
	}
	X86_COUNT(1);
	return down_tail(c);
}

/* ---- sliding past a wall */

/* up or down is blocked at feet row `row` of the target (`setup`: the
 * instructions that make the call's arguments, one more for up): the cells
 * left of the sprite and its own first cell, then its last cell and the
 * one right of it, say which side is open */
static int slide_sideways(const struct mv *c, uint32_t row, int setup)
{
	uint32_t a = cell_addr(c, (rd16(c->sp + S_X) - 1) & 0xffff, row);
	X86_COUNT(setup + 3);
	if (!wall(a))
		return left_face(c);
	a += 2;
	X86_COUNT(3);
	if (!wall(a))
		return left_face(c);
	uint32_t w = rd8(c->sp + S_WIDTH);
	a = (a & 0xffff0000u) | ((a + 2 * ((w - 1) & 0xffff)) & 0xffff);
	X86_COUNT(7);
	if (!wall(a))
		return right_face(c);
	a += 2;
	X86_COUNT(3);
	if (!wall(a))
		return right_face(c);
	X86_COUNT(3);
	return 0;
}

/* left or right is blocked at column `x` (the cell the sprite meant to
 * enter): the rows two and one above the feet row say whether to slide up,
 * the rows below it whether to slide down (`setup` as above) */
static int slide_vertically(const struct mv *c, uint32_t x, int setup)
{
	uint32_t a = cell_addr(c, x, foot_row(c, -3));
	X86_COUNT(setup + 3);
	if (!wall(a))
		return up_face(c);
	uint32_t step = rd16(c->m + M_MAP_W) * 2;
	a += step;
	X86_COUNT(5);
	if (!wall(a))
		return up_face(c);
	a += step;
	a += step;
	X86_COUNT(4);
	if (!wall(a))
		return down_face(c);
	a += step;
	X86_COUNT(3);
	if (!wall(a))
		return down_face(c);
	X86_COUNT(3);
	return 0;
}

/* ---- the movers */

static int mv_up(const struct mv *c)
{
	face(c, 0);
	X86_COUNT(3);
	if (rd16(c->sp + S_Y) == 0)
		return slide_sideways(c, foot_row(c, -2), 7);
	if (blocked(c))
		return 0;
	X86_COUNT(2);
	if (!(rd8(c->sp + S_FLAGS) & SPRITE_SLIDE)) {
		uint32_t a = cell_addr(c, rd16(c->sp + S_X), foot_row(c, -2));
		X86_COUNT(6 + 2);
		for (uint32_t n = rd8(c->sp + S_WIDTH); n; n--) {
			X86_COUNT(5);
			if (wall(a))
				return slide_sideways(c, foot_row(c, -2), 7);
			a += 2;
		}
	}
	return up_tail(c);
}

static int mv_down(const struct mv *c)
{
	face(c, 2);
	X86_COUNT(6);
	if (((rd16(c->sp + S_Y) + rd8(c->sp + S_HEIGHT)) & 0xffff) == rd16(c->m + M_MAP_H))
		return slide_sideways(c, foot_row(c, 0), 6);
	if (blocked(c))
		return 0;
	X86_COUNT(2);
	if (!(rd8(c->sp + S_FLAGS) & SPRITE_SLIDE)) {
		uint32_t a = cell_addr(c, rd16(c->sp + S_X), foot_row(c, 0));
		X86_COUNT(5 + 2);
		for (uint32_t n = rd8(c->sp + S_WIDTH); n; n--) {
			X86_COUNT(5);
			if (wall(a))
				return slide_sideways(c, foot_row(c, 0), 6);
			a += 2;
		}
	}
	return down_tail(c);
}

static int mv_left(const struct mv *c)
{
	face(c, 4);
	X86_COUNT(3);
	if (rd16(c->sp + S_X) == 0)
		return slide_vertically(c, (rd16(c->sp + S_X) - 1) & 0xffff, 7);
	if (blocked(c))
		return 0;
	X86_COUNT(2);
	if (!(rd8(c->sp + S_FLAGS) & SPRITE_SLIDE)) {
		uint32_t x = (rd16(c->sp + S_X) - 1) & 0xffff;
		uint32_t a = cell_addr(c, x, foot_row(c, -1));
		X86_COUNT(7 + 3);
		if (wall(a))
			return slide_vertically(c, x, 7);
	}
	return left_tail(c);
}

static int mv_right(const struct mv *c)
{
	face(c, 6);
	X86_COUNT(6);
	/* the edge test adds the height (0x404cb8), the cell tested below the width */
	if (((rd16(c->sp + S_X) + rd8(c->sp + S_HEIGHT)) & 0xffff) == rd16(c->m + M_MAP_W))
		return slide_vertically(c, (rd16(c->sp + S_X) + rd8(c->sp + S_WIDTH)) & 0xffff, 9);
	if (blocked(c))
		return 0;
	X86_COUNT(2);
	if (!(rd8(c->sp + S_FLAGS) & SPRITE_SLIDE)) {
		uint32_t x = (rd16(c->sp + S_X) + rd8(c->sp + S_WIDTH)) & 0xffff;
		uint32_t a = cell_addr(c, x, foot_row(c, -1));
		X86_COUNT(9 + 3);
		if (wall(a))
			return slide_vertically(c, x, 9);
	}
	return right_tail(c);
}

/* the register layer: ESI the sprite, EDI the selector; AL the result */
static void run(int (*mover)(const struct mv *))
{
	struct mv c = { cpu.r[ESI], SEG1 + cpu.r[ESI], SEG1 + cpu.r[EDI] };
	int r = mover(&c);
	set8l(EAX, (uint32_t)r);
}

void game_map_move_up(void) { run(mv_up); }
void game_map_move_down(void) { run(mv_down); }
void game_map_move_left(void) { run(mv_left); }
void game_map_move_right(void) { run(mv_right); }
