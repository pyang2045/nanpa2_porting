/*
 * Differential fuzzing of the map units (NANPA2_FUZZ, src/game/game.c).
 *
 * An explore run walks one town, so most branches of the movers, the
 * collision test, the walk planner and the sprite scripts never run. With
 * NANPA2_FUZZ=N each of the first NANPA2_FUZZ_CALLS compared calls of a map
 * unit is compared N more times from random states: the driver snapshots
 * the game's memory and the CPU, the functions below make the state
 * random (sprites, walls around them, view limits, the walk grid, the
 * arguments of a 04 26 op), compare() runs the original and the C from it,
 * and everything is put back, so the game goes on with its real state.
 * The states are random but not wild: positions stay in the map, no
 * sprite is of kind 6 (that is native and pumps), pattern data is not
 * touched. Only the diff builds use this.
 */
#include "game/map.h"

/* a xorshift generator */
static uint32_t rnd(uint32_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 17;
	*s ^= *s << 5;
	return *s;
}

static uint32_t below(uint32_t *s, uint32_t n)
{
	return n ? rnd(s) % n : 0;
}

static int chance(uint32_t *s, uint32_t percent)
{
	return below(s, 100) < percent;
}

struct world {
	uint32_t m, sel, mw, mh, sprites, n; /* M's address, selector, map size, sprite table offset, count */
};

static struct world world(void)
{
	struct world w;
	w.sel = rd16(MAP_SEL);
	w.m = SEG1 + w.sel;
	w.mw = rd16(w.m + M_MAP_W);
	w.mh = rd16(w.m + M_MAP_H);
	w.sprites = rd16(w.m + M_SPRITES);
	for (w.n = 0; w.n < 16 && rd8(SEG1 + w.sprites + 16 * w.n) != SPRITE_END; w.n++)
		;
	return w;
}

/* a coordinate: now and then right at an edge */
static uint32_t coord(uint32_t *s, uint32_t size)
{
	if (size <= 1)
		return 0;
	uint32_t r = below(s, 10);
	return r == 0 ? 0 : r == 1 ? size - 1 : below(s, size);
}

static void fuzz_sprite(uint32_t *s, const struct world *w, uint32_t k)
{
	uint32_t sp = SEG1 + w->sprites + 16 * k;
	uint32_t f = 0;
	for (uint32_t bit = 1; bit <= 0x80; bit <<= 1)
		if (chance(s, bit == SPRITE_ACTIVE || bit == SPRITE_DRAWN ? 70 : 25))
			f |= bit;
	wr8(sp + S_FLAGS, f & 0x7f ? f : f | 1);
	wr16(sp + S_X, coord(s, w->mw));
	wr16(sp + S_Y, coord(s, w->mh));
	wr8(sp + S_FRAME, below(s, 8));
	wr8(sp + S_WIDTH, 1 + below(s, 3));
	wr8(sp + S_HEIGHT, 1 + below(s, 3));
}

static void fuzz_sprites(uint32_t *s, const struct world *w)
{
	for (uint32_t k = 0; k < w->n; k++)
		fuzz_sprite(s, w, k);
}

/* the wall bits of the layer around a cell */
static void fuzz_walls(uint32_t *s, const struct world *w, uint32_t cx, uint32_t cy, uint32_t percent)
{
	uint32_t layer = rd16(w->m + M_LAYER);
	for (int dy = -4; dy <= 5; dy++)
		for (int dx = -4; dx <= 5; dx++) {
			int x = (int)cx + dx, y = (int)cy + dy;
			if (x < 0 || y < 0 || (uint32_t)x >= w->mw || (uint32_t)y >= w->mh)
				continue;
			uint32_t a = SEG1 + layer + 4 + 2 * ((uint32_t)y * w->mw + (uint32_t)x) + 1;
			uint32_t v = rd8(a) & 0x7f;
			wr8(a, chance(s, percent) ? v | 0x80 : v);
		}
}

/* the view and its limits */
static void fuzz_view(uint32_t *s, const struct world *w)
{
	wr16(w->m + M_VIEW_W, 3 + below(s, 12));
	wr16(w->m + M_VIEW_H, 3 + below(s, 10));
	wr16(w->m + M_VIEW_X, below(s, w->mw));
	wr16(w->m + M_VIEW_Y, below(s, w->mh));
	wr16(w->m + M_MARGIN_L, below(s, 5));
	wr16(w->m + M_MARGIN_T, below(s, 5));
	wr16(w->m + M_MARGIN_R, below(s, 5));
	wr16(w->m + M_MARGIN_B, below(s, 5));
	wr16(w->m + M_MIN_X, below(s, 4));
	wr16(w->m + M_MIN_Y, below(s, 4));
	wr16(w->m + M_MAX_X, w->mw - below(s, 4));
	wr16(w->m + M_MAX_Y, w->mh - below(s, 4));
}

static uint32_t pick_sprite(uint32_t *s, const struct world *w)
{
	return w->sprites + 16 * below(s, w->n);
}

static void fuzz_mover(uint32_t *s)
{
	struct world w = world();
	if (!w.n || w.mw < 4 || w.mh < 4)
		return;
	fuzz_sprites(s, &w);
	uint32_t sp = pick_sprite(s, &w);
	fuzz_view(s, &w);
	fuzz_walls(s, &w, rd16(SEG1 + sp + S_X), rd16(SEG1 + sp + S_Y), chance(s, 50) ? 15 : 40);
	cpu.r[ESI] = sp;
	cpu.r[EDI] = w.sel;
}

static void fuzz_collision(uint32_t *s)
{
	struct world w = world();
	if (!w.n)
		return;
	fuzz_sprites(s, &w);
	wr16(MAP_ARGS, below(s, w.n));
}

static void fuzz_area(uint32_t *s)
{
	struct world w = world();
	if (!w.n)
		return;
	fuzz_sprites(s, &w);
	uint32_t k = below(s, w.n);
	wr16(MAP_ARGS, k);
	/* put the sprite in an area now and then */
	uint32_t a = rd16(w.m + M_AREAS), cnt = 0;
	while (cnt < 32 && rd16(SEG1 + a + 10 * cnt) != 0xffff)
		cnt++;
	if (cnt && chance(s, 70)) {
		uint32_t r = SEG1 + a + 10 * below(s, cnt), sp = SEG1 + w.sprites + 16 * k;
		uint32_t x0 = rd16(r), y0 = rd16(r + 2), x1 = rd16(r + 4), y1 = rd16(r + 6);
		wr16(sp + S_X, x0 + (x1 >= x0 ? below(s, x1 - x0 + 1) : 0));
		wr16(sp + S_Y, y0 + (y1 >= y0 ? below(s, y1 - y0 + 1) : 0));
	}
}

/* the walk grid around a sprite: marks, walls and free cells */
static void fuzz_grid(uint32_t *s, const struct world *w, uint32_t cx, uint32_t cy)
{
	uint32_t base = rd16(MAP_WALK_BASE);
	for (int dy = -3; dy <= 3; dy++)
		for (int dx = -3; dx <= 3; dx++) {
			int x = (int)cx + dx, y = (int)cy + dy;
			if (x < 0 || y < 0 || (uint32_t)x >= w->mw || (uint32_t)y >= w->mh)
				continue;
			uint32_t r = below(s, 10), v;
			v = r < 3 ? 0xff : r < 5 ? 0xfe : r < 6 ? 0xfd : 1 + below(s, 0xfc);
			wr8(SEG1 + base + (uint32_t)y * w->mw + (uint32_t)x, v);
		}
}

static void fuzz_walk_step(uint32_t *s)
{
	struct world w = world();
	if (!w.n || w.mw < 4 || w.mh < 4)
		return;
	fuzz_sprites(s, &w);
	fuzz_view(s, &w);
	uint32_t sp = pick_sprite(s, &w);
	uint32_t x = rd16(SEG1 + sp + S_X), y = rd16(SEG1 + sp + S_Y);
	fuzz_walls(s, &w, x, y, 20);
	fuzz_grid(s, &w, x, y);
	cpu.r[ESI] = sp;
	cpu.r[EDI] = w.sel;
}

static void fuzz_plan_walk(uint32_t *s)
{
	struct world w = world();
	if (!w.n || w.mw < 4 || w.mh < 4)
		return;
	fuzz_sprites(s, &w);
	uint32_t sp = pick_sprite(s, &w);
	uint32_t x = rd16(SEG1 + sp + S_X), y = rd16(SEG1 + sp + S_Y);
	uint32_t gx = coord(s, w.mw), gy = coord(s, w.mh);
	/* the goal near the sprite half the time, so that routes exist */
	if (chance(s, 50)) {
		gx = (x + below(s, 9) + w.mw - 4) % w.mw;
		gy = (y + below(s, 9) + w.mh - 4) % w.mh;
	}
	fuzz_walls(s, &w, x, y, 15);
	fuzz_walls(s, &w, gx, gy, 15);
	wr8(MAP_AUTOWALK, chance(s, 8) ? 0xff : 0);
	wr8(0x435e61u, below(s, 2));
	cpu.r[ESI] = sp;
	cpu.r[ECX] = gx;
	cpu.r[EDX] = (cpu.r[EDX] & 0xffff0000u) | gy;
}

static void fuzz_sprite_step(uint32_t *s)
{
	struct world w = world();
	if (!w.n)
		return;
	fuzz_sprites(s, &w);
	fuzz_view(s, &w);
	uint32_t sp = pick_sprite(s, &w);
	uint32_t p = SEG1 + sp;
	uint32_t donor = SEG1 + pick_sprite(s, &w);
	wr8(p + S_FLAGS, rd8(p + S_FLAGS) | (chance(s, 85) ? SPRITE_ACTIVE : 0));
	wr8(p + S_SCRIPT, rd8(donor + S_SCRIPT));
	wr8(p + S_SCRIPT_PC, below(s, 6));
	static const uint8_t kinds[] = { 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 13 };
	wr8(p + S_KIND, kinds[below(s, sizeof kinds)]);
	uint32_t r = below(s, 10);
	wr8(p + S_COUNT, r < 5 ? 0 : r < 8 ? 1 + below(s, 4) : 0xf);
	for (uint32_t i = 0; i < 5 * HISTORY_LEN; i++)
		wr8(HISTORY + i, below(s, 256));
	wr8(0x435e60u, below(s, 256));
	fuzz_walls(s, &w, rd16(p + S_X), rd16(p + S_Y), 20);
	fuzz_grid(s, &w, rd16(p + S_X), rd16(p + S_Y));
	wr8(MAP_AUTOWALK, chance(s, 20) ? 0xff : 0);
	wr8(0x435e61u, below(s, 2));
	cpu.r[ESI] = sp;
	cpu.r[EDI] = w.sel;
	for (int tries = 0; tries < 8 && game_native_sprite_step(); tries++) {
		/* kind 6 is native: take another script position or kind */
		wr8(p + S_SCRIPT_PC, below(s, 6));
		wr8(p + S_COUNT, 1);
		wr8(p + S_KIND, kinds[below(s, sizeof kinds)]);
	}
}

/* ---- the 04 26 handler: an argument list for a sub-op that needs no
 * drawing, written over the script at the handler's ESI */

static uint32_t put_value(uint32_t pc, uint32_t v)
{
	if (v < 16) {
		vm_set_script_byte(pc++, 0x30 + v);
	} else if (v < 64) {
		vm_set_script_byte(pc++, 0x07);
		vm_set_script_byte(pc++, v << 2);
	} else {
		vm_set_script_byte(pc++, 0x08);
		vm_set_script_byte(pc++, (v >> 6) << 2);
		vm_set_script_byte(pc++, (v & 63) << 2);
	}
	vm_set_script_byte(pc++, 0x03);
	return pc;
}

static void fuzz_op_map(uint32_t *s)
{
	static const uint8_t ops[] = { 0, 1, 10, 11, 13, 14, 15, 16, 30, 32 };
	struct world w = world();
	if (!w.n || w.mw < 6 || w.mh < 6)
		return;
	uint32_t op = ops[below(s, sizeof ops)];
	uint32_t pc = cpu.r[ESI];
	fuzz_sprites(s, &w);
	fuzz_walls(s, &w, below(s, w.mw), below(s, w.mh), 15);
	pc = put_value(pc, op);
	uint32_t args[6] = { 0, 0, 0, 0, 0, 0 };
	uint32_t nargs = 0;
	switch (op) {
	case 10:
	case 11:
		args[0] = 0;
		nargs = 1;
		break;
	case 13:
		args[0] = below(s, w.n);
		args[1] = below(s, 8);
		nargs = 2;
		break;
	case 14:
		args[0] = below(s, w.n);
		args[1] = below(s, 2);
		nargs = 2;
		break;
	case 15: { /* a run-length layer of 2..5 by 2..5 cells at a scratch offset */
		/* a value in an argument has 12 bits: look for 64 unused bytes
		 * (zeros) below 0x1000, else the op is not tried */
		uint32_t at = 0;
		for (uint32_t o = 0x40; o < 0xfc0 && !at; o += 16) {
			uint32_t k = 0;
			while (k < 64 && rd8(SEG1 + o + k) == 0)
				k++;
			if (k == 64)
				at = o;
		}
		if (!at) {
			args[0] = 0;
			op = 32; /* stop the walk instead */
			pc = cpu.r[ESI];
			pc = put_value(pc, op);
			break;
		}
		uint32_t lw = 2 + below(s, 4), lh = 2 + below(s, 4), left = lw * lh;
		wr16(SEG1 + at, lw);
		wr16(SEG1 + at + 2, lh);
		uint32_t p = at + 4;
		while (left) {
			uint32_t tile = below(s, 0x400);
			if (left > 2 && chance(s, 40)) {
				uint32_t run = 2 + below(s, left - 1);
				wr16(SEG1 + p, tile | 0x4000);
				wr8(SEG1 + p + 2, run);
				p += 3;
				left -= run;
			} else {
				wr16(SEG1 + p, tile);
				p += 2;
				left--;
			}
		}
		args[0] = at;
		nargs = 1;
		break;
	}
	case 16: { /* copy a rectangle of cells */
		uint32_t x0 = below(s, w.mw / 2), y0 = below(s, w.mh / 2);
		args[0] = x0;
		args[1] = y0;
		args[2] = x0 + below(s, w.mw / 2);
		args[3] = y0 + below(s, w.mh / 2);
		args[4] = below(s, w.mw / 2);
		args[5] = below(s, w.mh / 2);
		nargs = 6;
		break;
	}
	case 30:
		args[0] = below(s, w.n);
		wr8(SEG1 + w.sprites + 16 * args[0] + S_DEST_X, below(s, w.mw));
		wr8(SEG1 + w.sprites + 16 * args[0] + S_DEST_Y, below(s, w.mh));
		nargs = 1;
		break;
	default:
		break;
	}
	for (uint32_t i = 0; i < nargs; i++) {
		vm_set_script_byte(pc++, 0x02);
		pc = put_value(pc, args[i]);
	}
	vm_set_script_byte(pc, 0x00);
	wr8(MAP_AUTOWALK, chance(s, 10) ? 0xff : 0);
	wr8(0x435e61u, 0);
}

/* the fuzz function of a unit, or NULL */
void (*game_map_fuzz_for(uint32_t addr))(uint32_t *)
{
	switch (addr) {
	case 0x404731: case 0x404937: case 0x404a9a: case 0x404c9f:
		return fuzz_mover;
	case 0x40500e:
		return fuzz_collision;
	case 0x404f8c:
		return fuzz_area;
	case 0x405691:
		return fuzz_walk_step;
	case 0x405966:
		return fuzz_plan_walk;
	case 0x40431b:
		return fuzz_sprite_step;
	case 0x4073ee:
		return fuzz_op_map;
	default:
		return NULL;
	}
}
