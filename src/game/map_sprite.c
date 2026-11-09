/*
 * The town map, part 5: the sprite scripts, 0x40431b.
 *
 * Every frame the map steps each sprite (sub-op 7 and 12, the loop in
 * map_pump.c calls this for each record). A sprite runs a movement script
 * (its script number, found through the script pointer table of M; script_pc
 * is the position in it): while it has steps left (count) it takes one in its
 * movement kind; when it has none it reads the next script byte, whose high
 * nibble is the kind and low nibble the count:
 *   1 up, 2 down, 3 left, 4 right, 5 and 7 stand (5 flips the step frame),
 *   6 the player (mouse and keys: map_pump.c, it pumps messages),
 *   8 left+up, 9 right+up, 10 left+down, 11 right+down, 13 walk to the
 *   sprite's destination (dest_x, dest_y) by the walk grid,
 *   0xc n: take the position of history record n (the followers),
 *   0xe n: a random one of the kinds 1-4 for n steps,
 *   0xf n: n < 8 set the frame to n, 8 show, 9 hide,
 *   0xff: back to the start of the script.
 * A count of 0xf never runs out. After a step the frame's step bit flips.
 * The movers are map_move.c's and the planner map_path.c's.
 *
 * `eax` is the AX leftover that sub-op 7 hands to the script as S: the
 * movers' results replace its low byte, the player's step leaves the frame it
 * started with.
 */
#include "game/map.h"

static void mover(uint32_t *eax, int r)
{
	*eax = (*eax & 0xffffff00u) | ((uint32_t)r & 0xffu);
}

/* the walk to the destination: the planner if no route is set, else one
 * step along it (kind 13, 0x4046f3) */
static void walk_to_destination(struct sprite *s, uint32_t *eax)
{
	if (MV(autowalk) == 0xff) {
		game_map_walk_step(s, eax);
		return;
	}
	MV_SET(approach, 1);
	game_map_plan_walk(s, s->dest_x, s->dest_y);
}

/* 0x404410: one step in the sprite's kind (its `call` and the stub's
 * `jmp 0x404448`, xor and ret are counted here) */
static void step(struct sprite *s, uint32_t *eax)
{
	uint32_t count = s->count;
	if (count != 0xf) {
		s->count = count - 1;
	}
	switch (s->kind) {
	case 1: mover(eax, game_map_move_up(s)); break;
	case 2: mover(eax, game_map_move_down(s)); break;
	case 3: mover(eax, game_map_move_left(s)); break;
	case 4: mover(eax, game_map_move_right(s)); break;
	case 5: break;
	case 6:
		/* the player's step (0x404450); the ret of the call of 0x404410 */
		game_map_player_step(s, eax);
		return;
	case 7:
		return; /* stands still, the frame stays */
	case 8:
		mover(eax, game_map_move_left(s));
		mover(eax, game_map_move_up(s));
		break;
	case 9:
		mover(eax, game_map_move_right(s));
		mover(eax, game_map_move_up(s));
		break;
	case 10:
		mover(eax, game_map_move_left(s));
		mover(eax, game_map_move_down(s));
		break;
	case 11:
		mover(eax, game_map_move_right(s));
		mover(eax, game_map_move_down(s));
		break;
	case 13: walk_to_destination(s, eax); break;
	default:
		game_trap(0x404410, "map: a sprite movement kind the C does not run");
	}
	s->frame ^= 1;
}

/* 0x40431b: one sprite's step */
void game_map_sprite_step(struct sprite *s, uint32_t *eax)
{
	const struct map_rec *m = map_rec();
	uint32_t fl = s->flags;
	if (!(fl & SPRITE_ACTIVE)) {
		return;
	}
	if (fl & SPRITE_OFF) {
		return;
	}
	if (s->count == 0) {
		uint32_t b;
		for (;;) {
			uint32_t t = (s->script * 2 + m->scripts) & 0xffff;
			uint32_t base = seg1_u16(t);
			b = seg1_u8(base + s->script_pc);
			if (b != 0xff)
				break;
			s->script_pc = 0;
		}
		s->script_pc = (s->script_pc + 1) & 0xff;
		uint32_t lo = b & 0xf, hi = b >> 4;
		if (hi == 0xf) {
			if (lo < 8) {
				s->frame = lo;
			} else if (lo == 8) {
				s->flags |= SPRITE_DRAWN;
			} else if (lo == 9) {
				s->flags &= (uint8_t)~SPRITE_DRAWN;
			} else {
			}
			return;
		}
		if (hi == 0xc) {
			const struct hist_rec *h = hist(lo);
			s->frame = h->frame;
			s->x = h->x;
			s->y = h->y;
			return;
		}
		if (hi == 0xe) {
			uint32_t r = MV(random_seed);
			uint32_t nr = ((((r << 2) | (r >> 7)) & 0xff) + r + 1) & 0xff;
			MV_SET(random_seed, nr);
			hi = (nr & 3) + 1;
		}
		s->kind = hi;
		if (lo == 0) {
			lo = 1;
		}
		s->count = lo;
	}
	step(s, eax);
}
