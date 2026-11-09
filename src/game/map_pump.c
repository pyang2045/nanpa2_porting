/*
 * The town map, part 6: the sub-ops that wait for the window, and the 04 26
 * handler.
 *
 * Three sub-ops of 04 26 pump messages: 12 (one map frame: step the sprites,
 * redraw, then the event check), 7 (step the sprites) and 5 (redraw every
 * cell, then the event check). The sprite loop reaches the player's step
 * (sprite kind 6), which pumps up to three times (the right button, the left
 * button, the keyboard), the event check pumps while the mouse button is held,
 * and sub-op 12 holds the frame while the screen's own button is down. These
 * are ordinary functions that call game_map_pump(); a WM_QUIT inside it
 * leaves for the catch of the handler's ctl_piece (ctl.h).
 *
 * The instruction counts follow the native paths (0x403a70 argument copy and
 * table call, 0x4042f7 sprite loop, 0x404450 the player, 0x4051c2 sub-op 12,
 * 0x405216 the event check, 0x403e30 sub-op 5, 0x405597 right click, 0x40609c
 * the pump), so the clock the instruction count drives is unchanged.
 *
 * What the script register S gets (the handler's tail writes AX there) is
 * whatever the last code leaves: sub-op 12 the event or 0xfffe while the
 * screen's button is held, sub-op 5 the unlock's result, sub-op 7 the AX
 * leftover of the sprite loop (see map_sprite.c), the others what map.c says.
 */
#include "game/map.h"

static uint32_t rol16(uint32_t v, unsigned n)
{
	v &= 0xffff;
	return ((v << n) | (v >> (16 - n))) & 0xffff;
}

/* ---- the pump 0x40609c ---- */

/* pushad, the message pump 0x401bf0, popad, BL = left button (Enter /
 * click) | right button (Esc) << 1, BH = right button << 1, CX, DX = the
 * mouse. On WM_QUIT the VM's stack is reset (0x404729) and the game ends
 * (ctl_quit). */
struct pump_state game_map_pump(void)
{
	COST(2); /* pushad, call 0x401bf0 */
	int ok = ctl_pump();
	COST(2);
	if (!ok)
		ctl_quit();
	uint32_t bits = RT(input_bits);
	uint32_t bl = (bits & 0x10) ? 1 : 0, bh = (bits & 0x20) ? 1 : 0;
	bh <<= 1;
	bl |= bh;
	COST(11);
	return (struct pump_state){ bl | bh << 8, SV(mouse_x), SV(mouse_y) };
}

/* ---- the right click 0x405597 ---- */

/* pump; if the mouse is inside the view, plan a walk of the player to the
 * cell under it (0x405966), else stop the walk */
static void right_click(struct sprite *player)
{
	COST(2); /* pushad, call */
	struct pump_state p = game_map_pump();
	const struct map_rec *m = map_rec();
	COST(1);
	uint32_t ax = (m->screen_x << 4) & 0xffff;
	COST(4);
	if (p.x < ax)
		goto outside;
	ax = ((m->screen_x + m->view_w - 1) << 4) & 0xffff;
	COST(6);
	if (ax < p.x)
		goto outside;
	ax = m->screen_y;
	COST(3);
	if (p.y < ax)
		goto outside;
	ax = (((m->view_h << 4) & 0xffff) + m->screen_y) & 0xffff;
	COST(5);
	if (ax < p.y)
		goto outside;
	uint32_t gx = (p.x >> 4) - m->screen_x + m->view_x;
	uint32_t gy = (((p.y - m->screen_y) & 0xffff) >> 4) + m->view_y;
	MV_SET(approach, 0);
	COST(8);
	game_map_plan_walk(player, gx, gy);
	COST(2);
	return;
outside:
	MV_SET(autowalk, 0);
	COST(3);
}

/* ---- the player 0x404450 (sprite kind 6) ---- */

/* the keyboard: pushad; call the message pump 0x401bf0; the four keys (left,
 * right, up, down), a mover each. Returns what the movers moved (0xff or 0) */
static uint32_t player_keys(struct sprite *s)
{
	COST(2);
	int ok = ctl_pump();
	COST(2);
	if (!ok)
		ctl_quit();
	COST(2);
	static const struct {
		uint32_t mask;
		int (*mover)(struct sprite *);
	} keys[4] = {
		{ 0x04, game_map_move_left }, { 0x08, game_map_move_right },
		{ 0x01, game_map_move_up }, { 0x02, game_map_move_down },
	};
	uint32_t ch = 0;
	for (int k = 0; k < 4; k++) {
		uint32_t al = 0;
		COST(4);
		if (RT(input_bits) & keys[k].mask) {
			al = (uint32_t)keys[k].mover(s) & 0xff;
			MV_SET(moved, 0xff);
			COST(2);
		}
		ch = (ch | al) & 0xff;
		COST(2);
	}
	return ch;
}

/* 0x404662: the step is over: if nothing moved the frame is left as it was;
 * else the walk stops, the frame's step bit flips and the history ring
 * shifts by one record, the old position first */
static void player_end(struct sprite *s, uint32_t ch, uint32_t frame0, uint32_t x0, uint32_t y0)
{
	COST(5);
	if (ch == 0) {
		COST(1); /* ret at 0x40444f */
		return;
	}
	MV_SET(autowalk, 0);
	s->frame ^= 1;
	COST(10);
	for (int i = 0; i < 9; i++) {
		*hist(8 - i + 1) = *hist(8 - i);
		COST(9);
		if (i < 8)
			POLL(0x40469c);
	}
	hist(0)->frame = frame0;
	hist(0)->x = x0;
	hist(0)->y = y0;
	COST(9);
}

/* 0x40453a / 0x404527 / 0x404514 / ...: the mover, then the end (the step
 * moved: ch = 1 whatever the mover said) */
static uint32_t click_mover(struct sprite *s, int (*mover)(struct sprite *))
{
	mover(s);
	MV_SET(moved, 0xff);
	COST(4);
	return 1;
}

static uint32_t click_none(void) /* 0x4045d7 */
{
	MV_SET(moved, 0xff);
	COST(3);
	return 0;
}

/* 0x40453a: the click is left or right of the player, or outside the margin
 * (the vertical test said no) */
static uint32_t click_sideways(const struct map_rec *m, struct sprite *s, struct pump_state p)
{
	MV_SET(scrolled, 0);
	uint32_t ax = (rol16(s->y - m->view_y, 4) + m->screen_y) & 0xffff;
	COST(7);
	uint32_t before = ax;
	ax = (ax - 0x30) & 0xffff;
	if (before < 0x30) { /* sub ax, 0x30 borrowed: jae not taken */
		ax = (ax + 0x30) & 0xffff;
		MV_SET(scrolled, 0xff);
		COST(2);
	}
	COST(2);
	if (p.y < ax)
		return click_none();
	ax = (ax + 0x20 + 0x60) & 0xffff;
	COST(4);
	if (MV(scrolled) != 0) {
		ax = (ax - 0x30) & 0xffff;
		COST(1);
	}
	COST(2);
	if (ax < p.y)
		return click_none();
	ax = rol16(s->x + m->screen_x - m->view_x, 4);
	COST(6);
	if (p.x < ax)
		return click_mover(s, game_map_move_left);
	ax = (ax + 0x20) & 0xffff;
	COST(3);
	if (p.x < ax)
		return click_none();
	return click_mover(s, game_map_move_right);
}

/* 0x4044b7: after the pump with the left button down: the mouse position
 * against the player's on the screen, then the vertical margins first.
 * Returns ch. */
static uint32_t player_click(struct sprite *s, struct pump_state p)
{
	const struct map_rec *m = map_rec();
	uint32_t ax = (s->x + m->screen_x - m->view_x) & 0xffff;
	ax = (rol16(ax, 4) - 0x30) & 0xffff;
	COST(8);
	if (p.x < ax)
		return click_sideways(m, s, p);
	ax = (ax + 0x20 + 0x60) & 0xffff;
	COST(4);
	if (ax < p.x)
		return click_sideways(m, s, p);
	ax = (rol16(s->y - m->view_y, 4) + m->screen_y) & 0xffff;
	COST(6);
	if (p.y < ax)
		return click_mover(s, game_map_move_up);
	ax = (ax + 0x20) & 0xffff;
	COST(3);
	if (p.y < ax)
		return click_sideways(m, s, p);
	return click_mover(s, game_map_move_down);
}

/* 0x404450: the player's step, entered from the sprite step. It saves the
 * frame, x and y (for the history record), then follows the walk grid, the
 * mouse (the game's own cursor on) or the arrow keys, pumping before each.
 * *eax: the frame it started with, the AX the native step leaves. */
void game_map_player_step(struct sprite *s, uint32_t *eax)
{
	MV_SET(moved, 0);
	uint32_t frame0 = s->frame, x0 = s->x, y0 = s->y;
	uint32_t ch;
	COST(9);
	if (RT(cursor_shown) == 0) {
		ch = player_keys(s);
	} else {
		struct pump_state p;
		COST(3); /* pushad */
		if (MV(autowalk) == 0xff) { /* 0x404497 */
			uint32_t scratch = 0;
			game_map_walk_step(s, &scratch);
			s->frame ^= 1;
			COST(2);
		} else {
			COST(1);
			p = game_map_pump(); /* 0x40448b: the right button starts a walk */
			COST(2);
			if (p.bx & 2) {
				COST(1);
				right_click(s);
				COST(1); /* jmp 0x4044a3 */
			}
		}
		COST(2); /* 0x4044a3: popad, call the pump */
		p = game_map_pump(); /* 0x4044a9: the left button; without it the keyboard */
		COST(2);
		if (!(p.bx & 1)) {
			ch = player_keys(s);
		} else {
			COST(1);
			p = game_map_pump();
			ch = player_click(s, p);
		}
	}
	player_end(s, ch, frame0, x0, y0);
	*eax = frame0;
}

/* ---- the event check 0x405216 ---- */

/* 0x405372: remember the player's facing; ax = the event (0xffff: none);
 * else wait for the button to go up (0x405399). Returns ax. */
static uint32_t event_end(uint32_t ax)
{
	const struct map_rec *m = map_rec();
	MV_SET(last_sprite, sprites_of(m)->frame >> 1);
	COST(10);
	if (ax == 0xffff) {
		COST(1);
		return ax;
	}
	for (;;) {
		COST(2);
		struct pump_state p = game_map_pump(); /* 0x40539f: until the button is up */
		COST(3);
		if (!(p.bx & 1))
			break;
		POLL(0x4053a3);
	}
	COST(1);
	return ax;
}

/* 0x40530b: the area under sprite 0 (0x404f8c) */
static uint32_t event_area(void)
{
	MV_SET(args[0], 0);
	COST(2);
	uint32_t ax = game_map_area_trigger(0);
	COST(3);
	if (ax == 0xffff) {
		MV_SET(last_area, 0xffff);
		COST(2);
		return event_end(ax);
	}
	COST(2);
	if (MV(last_area) != ax) {
		MV_SET(last_area, ax);
		ax = (ax + 0x32) & 0xffff;
		COST(3);
		return event_end(ax);
	}
	/* 0x405340 */
	ax = sprites_of(map_rec())->frame >> 1;
	COST(7);
	if (ax == MV(last_sprite)) {
		ax = 0xffff;
		COST(1);
	} else {
		ax = (MV(last_area) + 0x32) & 0xffff;
		COST(3);
	}
	return event_end(ax);
}

/* 0x4052f1: the click (bx) is the one before: look at the area, else it is a new click */
static uint32_t event_click(uint32_t bx)
{
	uint32_t ax = bx & 0xffff;
	COST(3);
	if (MV(last_click) == ax)
		return event_area();
	MV_SET(last_click, ax);
	COST(2);
	return event_end(ax);
}

/* 0x4052e1: the click is on the player */
static uint32_t event_on_player(uint32_t ax)
{
	MV_SET(last_click, ax);
	COST(3);
	return event_end(ax);
}

/* 0x405273: after the second pump (button down at the contact): is the
 * mouse on the player's cell? ax = the collision test's result */
static uint32_t event_second(struct pump_state p, uint32_t ax)
{
	const struct map_rec *m = map_rec();
	const struct sprite *s = sprites_of(m);
	uint32_t a = (rol16(s->x + m->screen_x - m->view_x, 4) - 0x30) & 0xffff;
	COST(9);
	int off = p.x < a;
	if (!off) {
		a = (a + 0x20 + 0x60) & 0xffff;
		COST(4);
		off = a < p.x;
		if (!off) {
			COST(1);
			return event_on_player(ax);
		}
	}
	a = (rol16(s->y - m->view_y, 4) + m->screen_y - 0x30) & 0xffff;
	COST(7);
	if (p.y < a) {
		COST(2); /* 0x4052ed */
		return event_click(ax);
	}
	a = (a + 0x20 + 0x60) & 0xffff;
	COST(4);
	if (a < p.y) {
		COST(2);
		return event_click(ax);
	}
	return event_on_player(ax);
}

/* 0x405216: collision test of sprite 0 (0x40500e); if it touches something,
 * pump and look at the mouse; then the area trigger. Returns the event (AX):
 * 0xffff none, else a code. */
uint32_t game_map_event_check(void)
{
	MV_SET(args[0], 0);
	COST(2);
	uint32_t ax = game_map_collision(0);
	COST(3);
	if (ax == 0xffff) {
		MV_SET(last_click, ax); /* 0x405305 */
		COST(1);
		return event_area();
	}
	COST(2);
	struct pump_state p = game_map_pump(); /* 0x405233 */
	COST(3);
	if (!(p.bx & 1)) {
		COST(4);
		if (RT(input_bits) == 0)
			return event_click(ax);
		COST(3);
		if (MV(moved) != 0xff)
			return event_click(ax);
		COST(1);
		MV_SET(last_click, ax); /* 0x4052e2 */
		COST(2);
		return event_end(ax);
	}
	COST(2);
	if (MV(moved) != 0xff)
		return event_click(p.bx);
	COST(2);
	p = game_map_pump();
	return event_second(p, ax);
}

static void event_check_only(void)
{
	game_map_event_check();
}

/* ---- the sprite loop 0x4042f7 and the sub-ops ---- */

/* 0x404305: step each sprite record up to the 0xff flag byte */
static void sprites_step(uint32_t *eax)
{
	uint32_t off = map_rec()->sprites;
	COST(2);
	for (;;) {
		COST(2);
		if (seg1_u8(off) == SPRITE_END) {
			COST(1);
			return;
		}
		COST(2);
		game_map_sprite_step(sprite_at(off), eax);
		off += sizeof(struct sprite);
		COST(3);
		POLL(0x404318);
	}
}

/* 0x4051c2: sub-op 12, one frame: step the sprites, redraw the cells that
 * changed (sub-ops 2, 3, 4), pump; a left click on the screen's own button
 * holds the frame until it is released; else the event check */
static uint32_t subop_frame(void)
{
	uint32_t eax = 12;
	MV_SET(moved, 0);
	COST(2);
	sprites_step(&eax);
	eax = game_map_fill_grid(eax);
	eax = game_map_overlay_sprites(eax);
	game_map_redraw_changed();
	COST(5);
	struct pump_state p = game_map_pump(); /* 0x4051e3 */
	COST(3);
	if (!(p.bx & 1))
		return game_map_event_check();
	COST(2);
	if (p.x < 0xf8)
		return game_map_event_check();
	COST(2);
	if (p.x >= 0x188)
		return game_map_event_check();
	COST(2);
	if (p.y < 0x120)
		return game_map_event_check();
	COST(2);
	if (p.y >= 0x140)
		return game_map_event_check();
	COST(3);
	for (;;) {
		p = game_map_pump(); /* 0x40520f: until the button is up */
		COST(3);
		if (!(p.bx & 1))
			break;
		POLL(0x405213);
		COST(2);
	}
	COST(1);
	return 0xfffe;
}

/* ---- the handler 0x4073ee ---- */

/* 04 26 n, args...: map sub-op n through 0x403a70, S = AX after it. Sub-ops
 * 4 and 5 first reset the animation slots (0x406dff). The handler's tail
 * (0x40740f: pop esi, mov [S], ax, jmp 0x4060f9) is here too. */
int vm_op_map(void)
{
	uint32_t pc = vm_pc, a;
	vm_parse_args(&pc, &a);
	COST(1);
	COST(2);
	if (a == 5 || a == 4) {
		game_anim_reset();
		COST(2);
	}
	COST(2);
	uint32_t ax;
	if (game_map_op_in_c(a)) {
		ax = game_map_op(a);
		COST(1); /* pop esi */
	} else {
		if (a != 5 && a != 7 && a != 12)
			x86_trap(0x403a7f, "04 26: sub-op outside the dispatch table");
		game_map_load_args();
		switch (a) {
		case 5: /* 0x403e30: redraw every cell, the event check, unlock */
			ax = game_map_redraw_all(event_check_only);
			break;
		case 7: /* 0x4042f7 */
			ax = 7;
			sprites_step(&ax);
			break;
		default:
			ax = subop_frame();
			break;
		}
		COST(1); /* the ret of 0x403a86 */
		COST(1); /* pop esi */
	}
	vm_pc = pc;
	vm_set_reg(VM_REG_S, ax);
	COST(2); /* mov [S], ax; jmp 0x4060f9 */
	POLL(0x407416);
	return -1;
}
