/*
 * The town map, part 3 (stage 5A): the four movers 0x404731 (up), 0x404937
 * (down), 0x404a9a (left) and 0x404c9f (right).
 *
 * A mover turns the sprite to face its direction, then tries to step one cell: it fails
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
 * The movers' result is AL (0xff moved, 0 blocked), the return value.
 */
#include "game/map.h"

struct mv {
	struct sprite *s;
	struct map_rec *m;
};

/* 0x404e06: the offset in segment 1 of the layer's tile word at (x, y),
 * 16-bit arithmetic */
static uint32_t cell_addr(const struct mv *c, uint32_t x, uint32_t y)
{
	uint32_t w2 = (c->m->map_w * 2) & 0xffff;
	return (((w2 * (y & 0xffff)) & 0xffff) + x + x + c->m->layer + 4) & 0xffff;
}

/* the wall bit of the tile word at offset a: its high byte's top bit */
static int wall(uint32_t a)
{
	return (seg1_u8(a + 1) & 0x80) != 0;
}

/* the sprite's frame: its step bit, then the direction * 2 (up 0, down 2,
 * left 4, right 6) */
static void face(const struct mv *c, uint32_t dir2)
{
	c->s->frame = (c->s->frame & 1) + dir2;
}

static int blocked(const struct mv *c)
{
	int r = game_map_collide(c->s) != -1;
	return r;
}

static uint32_t foot_row(const struct mv *c, int adjust)
{
	return (c->s->y + c->s->height + adjust) & 0xffff;
}

/* ---- the tails: step, then scroll the view if the sprite is near its edge */

/* the scroll test: with d the distance from the margin, `far` means the
 * view stays, else it moves unless it is at its limit (`at_limit`) */
static int tail_end(int scrolled, int at_limit)
{
	/* mov al, -1; ret, and the dec + jmp of a scroll */
	return 0xff;
}

static int up_tail(const struct mv *c)
{
	uint32_t y = (c->s->y - 1) & 0xffff;
	c->s->y = y;
	if (!(c->s->flags & SPRITE_SCROLL))
		return tail_end(0, 0);
	uint32_t vy = c->m->view_y;
	uint32_t d = ((c->s->y - vy + 1) & 0xffff);
	uint32_t margin = c->m->margin_t;
	if (d > margin)
		return tail_end(0, 0);
	vy = c->m->view_y;
	if (vy == c->m->min_y)
		return tail_end(0, 0);
	c->m->view_y = (vy - 1) & 0xffff;
	return tail_end(1, 0);
}

static int down_tail(const struct mv *c)
{
	uint32_t y = (c->s->y + 1) & 0xffff;
	c->s->y = y;
	if (!(c->s->flags & SPRITE_SCROLL))
		return tail_end(0, 0);
	uint32_t vy = c->m->view_y;
	uint32_t d = (c->s->y + c->s->height - vy + c->m->margin_b - 1) & 0xffff;
	if (d < c->m->view_h)
		return tail_end(0, 0);
	vy = (c->m->view_y + c->m->view_h - 1) & 0xffff;
	if (vy == c->m->max_y)
		return tail_end(0, 0);
	c->m->view_y = (c->m->view_y + 1) & 0xffff;
	return tail_end(1, 0);
}

static int left_tail(const struct mv *c)
{
	uint32_t x = (c->s->x - 1) & 0xffff;
	c->s->x = x;
	if (!(c->s->flags & SPRITE_SCROLL))
		return tail_end(0, 0);
	uint32_t vx = c->m->view_x;
	uint32_t d = ((c->s->x - vx + 1) & 0xffff);
	uint32_t margin = c->m->margin_l;
	if (d > margin)
		return tail_end(0, 0);
	vx = c->m->view_x;
	if (vx == c->m->min_x)
		return tail_end(0, 0);
	c->m->view_x = (vx - 1) & 0xffff;
	return tail_end(1, 0);
}

static int right_tail(const struct mv *c)
{
	uint32_t x = (c->s->x + 1) & 0xffff;
	c->s->x = x;
	if (!(c->s->flags & SPRITE_SCROLL))
		return tail_end(0, 0);
	uint32_t vx = c->m->view_x;
	uint32_t d = (c->s->x + c->s->width - vx + c->m->margin_r - 1) & 0xffff;
	if (d < c->m->view_w)
		return tail_end(0, 0);
	vx = (c->m->view_x + c->m->view_w - 1) & 0xffff;
	if (vx == c->m->max_x)
		return tail_end(0, 0);
	c->m->view_x = (c->m->view_x + 1) & 0xffff;
	return tail_end(1, 0);
}

/* ---- turning to a side (or up or down) and trying the step there */

static int left_face(const struct mv *c)
{
	face(c, 4);
	if (c->s->x == 0) {
		return 0;
	}
	if (blocked(c))
		return 0;
	uint32_t a = cell_addr(c, (c->s->x - 1) & 0xffff, foot_row(c, -1));
	if (wall(a)) {
		return 0;
	}
	return left_tail(c);
}

static int right_face(const struct mv *c)
{
	face(c, 6);
	/* the native test adds the sprite's height, not its width (0x4048d7) */
	if (((c->s->x + c->s->height) & 0xffff) == c->m->map_w) {
		return 0;
	}
	if (blocked(c))
		return 0;
	uint32_t x = (c->s->x + c->s->width) & 0xffff;
	uint32_t a = cell_addr(c, x, foot_row(c, -1));
	if (wall(a)) {
		return 0;
	}
	return right_tail(c);
}

/* up, entered from a sideways slide: step up if the footprint's feet row
 * is free there */
static int up_face(const struct mv *c)
{
	c->s->frame = c->s->frame & 1;
	if (c->s->y == 0) {
		return 0;
	}
	if (blocked(c))
		return 0;
	uint32_t a = cell_addr(c, c->s->x, foot_row(c, -2));
	for (uint32_t n = c->s->width; n; n--) {
		if (wall(a)) {
			return 0;
		}
		a += 2;
	}
	return up_tail(c);
}

static int down_face(const struct mv *c)
{
	face(c, 2);
	if (((c->s->y + c->s->height) & 0xffff) == c->m->map_h) {
		return 0;
	}
	if (blocked(c))
		return 0;
	uint32_t a = cell_addr(c, c->s->x, foot_row(c, 0));
	for (uint32_t n = c->s->width; n; n--) {
		if (wall(a)) {
			return 0;
		}
		a += 2;
	}
	return down_tail(c);
}

/* ---- sliding past a wall */

/* up or down is blocked at feet row `row` of the target (`setup`: the
 * instructions that make the call's arguments, one more for up): the cells
 * left of the sprite and its own first cell, then its last cell and the
 * one right of it, say which side is open */
static int slide_sideways(const struct mv *c, uint32_t row, int setup)
{
	uint32_t a = cell_addr(c, (c->s->x - 1) & 0xffff, row);
	if (!wall(a))
		return left_face(c);
	a += 2;
	if (!wall(a))
		return left_face(c);
	uint32_t w = c->s->width;
	a = (a & 0xffff0000u) | ((a + 2 * ((w - 1) & 0xffff)) & 0xffff);
	if (!wall(a))
		return right_face(c);
	a += 2;
	if (!wall(a))
		return right_face(c);
	return 0;
}

/* left or right is blocked at column `x` (the cell the sprite meant to
 * enter): the rows two and one above the feet row say whether to slide up,
 * the rows below it whether to slide down (`setup` as above) */
static int slide_vertically(const struct mv *c, uint32_t x, int setup)
{
	uint32_t a = cell_addr(c, x, foot_row(c, -3));
	if (!wall(a))
		return up_face(c);
	uint32_t step = c->m->map_w * 2;
	a += step;
	if (!wall(a))
		return up_face(c);
	a += step;
	a += step;
	if (!wall(a))
		return down_face(c);
	a += step;
	if (!wall(a))
		return down_face(c);
	return 0;
}

/* ---- the movers */

static int mv_up(const struct mv *c)
{
	face(c, 0);
	if (c->s->y == 0)
		return slide_sideways(c, foot_row(c, -2), 7);
	if (blocked(c))
		return 0;
	if (!(c->s->flags & SPRITE_SLIDE)) {
		uint32_t a = cell_addr(c, c->s->x, foot_row(c, -2));
		for (uint32_t n = c->s->width; n; n--) {
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
	if (((c->s->y + c->s->height) & 0xffff) == c->m->map_h)
		return slide_sideways(c, foot_row(c, 0), 6);
	if (blocked(c))
		return 0;
	if (!(c->s->flags & SPRITE_SLIDE)) {
		uint32_t a = cell_addr(c, c->s->x, foot_row(c, 0));
		for (uint32_t n = c->s->width; n; n--) {
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
	if (c->s->x == 0)
		return slide_vertically(c, (c->s->x - 1) & 0xffff, 7);
	if (blocked(c))
		return 0;
	if (!(c->s->flags & SPRITE_SLIDE)) {
		uint32_t x = (c->s->x - 1) & 0xffff;
		uint32_t a = cell_addr(c, x, foot_row(c, -1));
		if (wall(a))
			return slide_vertically(c, x, 7);
	}
	return left_tail(c);
}

static int mv_right(const struct mv *c)
{
	face(c, 6);
	/* the edge test adds the height (0x404cb8), the cell tested below the width */
	if (((c->s->x + c->s->height) & 0xffff) == c->m->map_w)
		return slide_vertically(c, (c->s->x + c->s->width) & 0xffff, 9);
	if (blocked(c))
		return 0;
	if (!(c->s->flags & SPRITE_SLIDE)) {
		uint32_t x = (c->s->x + c->s->width) & 0xffff;
		uint32_t a = cell_addr(c, x, foot_row(c, -1));
		if (wall(a))
			return slide_vertically(c, x, 9);
	}
	return right_tail(c);
}

static int run(int (*mover)(const struct mv *), struct sprite *s)
{
	struct mv c = { s, map_rec() };
	return mover(&c) & 0xff;
}

int game_map_move_up(struct sprite *s) { return run(mv_up, s); }
int game_map_move_down(struct sprite *s) { return run(mv_down, s); }
int game_map_move_left(struct sprite *s) { return run(mv_left, s); }
int game_map_move_right(struct sprite *s) { return run(mv_right, s); }

