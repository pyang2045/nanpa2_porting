/*
 * The town map, part 6 (stage 6): the chain that pumps messages.
 *
 * Three sub-ops of the 04 26 handler wait for the window: 12 (one map frame:
 * step the sprites, redraw, then the event check), 7 (step the sprites) and
 * 5 (redraw every cell, then the event check). Their native code calls the
 * pump 0x40609c from ten places, and a WM_QUIT in it resets ESP to the VM's
 * top frame, so the chain cannot be a C function that returns: it is jump
 * units joined by call exits (the technique of loop.c / ops_vm.c). The
 * handler 0x4073ee runs the chain from the opcode down to the first pump;
 * each pump site's return address is a piece that goes on to the next pump
 * or, at the end, back out through the native return addresses to the
 * handler's tail 0x40740f.
 *
 * The native call frames are real: the pieces push the return addresses the
 * native calls push (0x40740f of the handler's `call 0x403a70`, 0x403a86 of
 * its table call, 0x4051ce / 0x404314 / 0x40440f / 0x403ed3 / 0x404495 of the
 * chain's calls), and a `ret` is go(): the return address says which part of
 * the chain runs next. A pump site's piece can therefore find itself under
 * any of the chain's callers, and its oracle ends where the handler's does
 * (game_chain_words finds the 0x40740f frame).
 *
 * Native code, by address (instruction counts follow the native paths, per
 * basic block):
 *   0x4073ee 04 26 handler, 0x403a70 argument copy and table call,
 *   0x4042f7 sprite loop (sub-op 7), 0x40431b / 0x404410 one sprite (in
 *   map_sprite.c), 0x404450 the player: cursor or keys,
 *   0x4051c2 sub-op 12, 0x405216 the event check, 0x403e30 sub-op 5,
 *   0x405597 right click, 0x40609c the pump.
 */
#include <string.h>

#include "game/map.h"
#include "game/vm.h"

#define PUMP 0x40609cu
#define PUMP_DONE 0x4060a2u
#define HLE_PUMP 0x401bf0u
#define TAIL 0x40740fu      /* the return address of the handler's call of 0x403a70 */
#define TAIL_PIECE 0x407410u /* after its `pop esi`: mov [S], ax; jmp 0x4060f9 */
#define VM_TOP_ESP 0x445cdcu
#define INPUT_BITS 0x445d8fu  /* u8: arrows 1 2 4 8, Enter / click 0x10, Esc / right click 0x20 */
#define MOUSE_X 0x4452e2u     /* u16 */
#define MOUSE_Y 0x4452e4u
#define CURSOR_SHOWN 0x445ce0u
#define SCROLLED 0x435572u    /* u8: the view scrolled in the player's step */
#define APPROACH 0x435e61u

#define CNT(n) X86_COUNT(n)

static uint32_t rol16(uint32_t v, unsigned n)
{
	v &= 0xffff;
	return ((v << n) | (v >> (16 - n))) & 0xffff;
}

#define AX16 (cpu.r[EAX] & 0xffffu)
#define CX16 (cpu.r[ECX] & 0xffffu)
#define DX16 (cpu.r[EDX] & 0xffffu)

/* `call 0x40609c` with the return address ra */
static uint32_t pump(uint32_t ra)
{
	push32(ra);
	return PUMP;
}

static uint32_t go(uint32_t ra);

/* ---- the pump 0x40609c ---- */

/* pushad, then the message pump 0x401bf0 (call exit; ra 0x4060a2) */
uint32_t game_pump(void)
{
	pushal();
	push32(PUMP_DONE);
	CNT(2);
	return HLE_PUMP;
}

/* After the message pump: WM_QUIT (EAX = 0) resets ESP to the VM's top
 * frame (0x404729: mov esp, [0x445cdc]; the pop and ret are finish_exit's);
 * else popad, BL = left button (Enter / click) | right button (Esc) << 1,
 * CX, DX = the mouse, ret. */
uint32_t game_pump_done(void)
{
	CNT(2);
	if (!cpu.r[EAX]) {
		cpu.r[ESP] = rd32(VM_TOP_ESP);
		CNT(1);
		return 0x40472f;
	}
	popal();
	uint32_t bits = rd8(INPUT_BITS);
	uint32_t bl = (bits & 0x10) ? 1 : 0, bh = (bits & 0x20) ? 1 : 0;
	bh <<= 1;
	bl |= bh;
	cpu.r[EBX] = bl | bh << 8;
	set16(ECX, rd16(MOUSE_X));
	set16(EDX, rd16(MOUSE_Y));
	CNT(11);
	return pop32();
}

/* ---- the right click 0x405597 ---- */

static uint32_t rc_begin(void) /* pushad; call 0x40609c */
{
	pushal();
	CNT(2);
	return pump(0x40559d);
}

/* 0x40559d: if the mouse is inside the view, plan a walk of the player to
 * the cell under it (0x405966), else stop the walk; popad, ret */
uint32_t game_rc_pumped(void)
{
	uint32_t ebx = rd16(MAP_SEL), m = SEG1 + ebx;
	cpu.r[EBX] = ebx;
	CNT(1);
	uint32_t ax = (rd16(m + M_SCREEN_X) << 4) & 0xffff;
	set16(EAX, ax);
	CNT(4);
	if (CX16 < ax)
		goto outside;
	ax = ((rd16(m + M_SCREEN_X) + rd16(m + M_VIEW_W) - 1) << 4) & 0xffff;
	set16(EAX, ax);
	CNT(6);
	if (ax < CX16)
		goto outside;
	ax = rd16(m + M_SCREEN_Y);
	set16(EAX, ax);
	CNT(3);
	if (DX16 < ax)
		goto outside;
	ax = (((rd16(m + M_VIEW_H) << 4) & 0xffff) + rd16(m + M_SCREEN_Y)) & 0xffff;
	set16(EAX, ax);
	CNT(5);
	if (ax < DX16)
		goto outside;
	set16(ECX, ((CX16 >> 4) - rd16(m + M_SCREEN_X) + rd16(m + M_VIEW_X)));
	set16(EDX, (((DX16 - rd16(m + M_SCREEN_Y)) & 0xffff) >> 4) + rd16(m + M_VIEW_Y));
	wr8(APPROACH, 0);
	CNT(8);
	game_map_plan_walk();
	popal();
	CNT(2);
	return go(pop32());
outside:
	wr8(MAP_AUTOWALK, 0);
	popal();
	CNT(3);
	return go(pop32());
}

/* ---- the player 0x404450 (sprite kind 6) ---- */

static uint32_t pl_end(void);
static uint32_t pl_4044a3(void);

/* 0x4045e5: the keyboard: pushad; call 0x401bf0 (the HLE pump itself) */
static uint32_t pl_kb_begin(void)
{
	pushal();
	push32(0x4045eb);
	CNT(2);
	return HLE_PUMP;
}

static void pl_moved(void)
{
	wr8(MAP_MOVED, 0xff);
}

/* 0x4045eb: after the pump: WM_QUIT, else popad and the four keys (left,
 * right, up, down: a mover each, CH collects their results) */
uint32_t game_pl_keys(void)
{
	CNT(2);
	if (!cpu.r[EAX]) {
		cpu.r[ESP] = rd32(VM_TOP_ESP);
		CNT(1);
		return 0x40472f;
	}
	popal();
	set8h(ECX, 0);
	CNT(2);
	static const struct {
		uint32_t mask;
		void (*mover)(void);
	} keys[4] = {
		{ 0x04, game_map_move_left }, { 0x08, game_map_move_right },
		{ 0x01, game_map_move_up }, { 0x02, game_map_move_down },
	};
	for (int k = 0; k < 4; k++) {
		push32(cpu.r[ECX]);
		set8l(EAX, 0);
		CNT(4);
		if (rd8(INPUT_BITS) & keys[k].mask) {
			keys[k].mover();
			pl_moved();
			CNT(2);
		}
		cpu.r[ECX] = pop32();
		set8h(ECX, (cpu.r[ECX] >> 8) | cpu.r[EAX]);
		CNT(2);
	}
	return pl_end();
}

/* 0x404662: the step is over: if nothing moved CH is 0 and the frame is
 * left as it was; else the walk stops (0x435573 = 0), the frame's step bit
 * flips and the history ring shifts by one record, the old position first */
static uint32_t pl_end(void)
{
	uint32_t ch = (cpu.r[ECX] >> 8) & 0xff;
	cpu.r[ECX] = pop32();
	cpu.r[EBX] = pop32();
	cpu.r[EAX] = pop32();
	CNT(5);
	if (ch == 0) {
		CNT(1); /* ret at 0x40444f */
		return go(pop32());
	}
	uint32_t sp = SEG1 + cpu.r[ESI];
	wr8(MAP_AUTOWALK, 0);
	wr8(sp + S_FRAME, rd8(sp + S_FRAME) ^ 1);
	CNT(10);
	for (int i = 0; i < 9; i++) {
		uint32_t from = HISTORY + 5 * (8 - i), to = from + 5;
		wr32(to, rd32(from));
		wr8(to + 4, rd8(from + 4));
		CNT(9);
		if (i < 8)
			X86_POLL(0x40469c);
	}
	wr8(HISTORY, cpu.r[EAX]);
	wr16(HISTORY + 1, cpu.r[EBX]);
	wr16(HISTORY + 3, cpu.r[ECX]);
	CNT(9);
	return go(pop32());
}

/* 0x40453a / 0x404527 / 0x404514 / ...: the mover and the end */
static uint32_t pl_mover(void (*mover)(void))
{
	mover();
	set8h(ECX, 1);
	pl_moved();
	CNT(4);
	return pl_end();
}

static uint32_t pl_none(void) /* 0x4045d7 */
{
	set8h(ECX, 0);
	pl_moved();
	CNT(3);
	return pl_end();
}

/* 0x40453a: the click is left or right of the player, or outside the margin
 * (the vertical test said no) */
static uint32_t pl_sideways(uint32_t m, uint32_t sp)
{
	wr8(SCROLLED, 0);
	uint32_t ax = (rol16(rd16(sp + S_Y) - rd16(m + M_VIEW_Y), 4) + rd16(m + M_SCREEN_Y)) & 0xffff;
	CNT(7);
	uint32_t before = ax;
	ax = (ax - 0x30) & 0xffff;
	if (before < 0x30) { /* sub ax, 0x30 borrowed: jae not taken */
		ax = (ax + 0x30) & 0xffff;
		wr8(SCROLLED, 0xff);
		CNT(2);
	}
	set16(EAX, ax);
	CNT(2);
	if (DX16 < ax)
		return pl_none();
	ax = (ax + 0x20 + 0x60) & 0xffff;
	CNT(4);
	if (rd8(SCROLLED) != 0) {
		ax = (ax - 0x30) & 0xffff;
		CNT(1);
	}
	set16(EAX, ax);
	CNT(2);
	if (ax < DX16)
		return pl_none();
	ax = rol16(rd16(sp + S_X) + rd16(m + M_SCREEN_X) - rd16(m + M_VIEW_X), 4);
	set16(EAX, ax);
	CNT(6);
	if (CX16 < ax)
		return pl_mover(game_map_move_left);
	ax = (ax + 0x20) & 0xffff;
	set16(EAX, ax);
	CNT(3);
	if (CX16 < ax)
		return pl_none();
	return pl_mover(game_map_move_right);
}

/* 0x4044b7: after the pump with the left button down: the mouse position
 * against the player's on the screen, then the vertical margins first */
uint32_t game_pl_click(void)
{
	uint32_t ebx = rd16(MAP_SEL), m = SEG1 + ebx;
	cpu.r[EBX] = ebx;
	uint32_t sp = SEG1 + cpu.r[ESI];
	uint32_t ax = (rd16(sp + S_X) + rd16(m + M_SCREEN_X) - rd16(m + M_VIEW_X)) & 0xffff;
	ax = (rol16(ax, 4) - 0x30) & 0xffff;
	set16(EAX, ax);
	CNT(8);
	if (CX16 < ax)
		return pl_sideways(m, sp);
	ax = (ax + 0x20 + 0x60) & 0xffff;
	set16(EAX, ax);
	CNT(4);
	if (ax < CX16)
		return pl_sideways(m, sp);
	ax = (rol16(rd16(sp + S_Y) - rd16(m + M_VIEW_Y), 4) + rd16(m + M_SCREEN_Y)) & 0xffff;
	set16(EAX, ax);
	CNT(6);
	if (DX16 < ax)
		return pl_mover(game_map_move_up);
	ax = (ax + 0x20) & 0xffff;
	set16(EAX, ax);
	CNT(3);
	if (DX16 < ax)
		return pl_sideways(m, sp);
	return pl_mover(game_map_move_down);
}

/* 0x404497 .. 0x4044a3: popad; call 0x40609c (left button?) */
static uint32_t pl_4044a3(void)
{
	popal();
	CNT(2);
	return pump(0x4044a9);
}

/* 0x40448b: after the first pump (inside the pushad of the cursor path):
 * the right button starts a walk (0x405597) */
uint32_t game_pl_pumped_a(void)
{
	set8l(EBX, cpu.r[EBX] & 2);
	CNT(2);
	if (!(cpu.r[EBX] & 2))
		return pl_4044a3();
	push32(0x404495);
	CNT(1);
	return rc_begin();
}

/* 0x4044a9: the left button; without it the keyboard path */
uint32_t game_pl_pumped_b(void)
{
	set8l(EBX, cpu.r[EBX] & 1);
	CNT(2);
	if (!(cpu.r[EBX] & 1))
		return pl_kb_begin();
	CNT(1);
	return pump(0x4044b7);
}

/* 0x404450: the player's step, entered from the sprite step (ESI = the
 * sprite, EDI = the map selector). It saves the frame, x and y (for the
 * history record), then follows the walk grid, the mouse (the game's own
 * cursor on) or the arrow keys, pumping before each. */
static uint32_t player_begin(void)
{
	uint32_t sp = SEG1 + cpu.r[ESI];
	wr8(MAP_MOVED, 0);
	cpu.r[EAX] = rd8(sp + S_FRAME);
	push32(cpu.r[EAX]);
	cpu.r[EBX] = rd16(sp + S_X);
	push32(cpu.r[EBX]);
	cpu.r[ECX] = rd16(sp + S_Y);
	push32(cpu.r[ECX]);
	CNT(9);
	if (rd8(CURSOR_SHOWN) == 0)
		return pl_kb_begin();
	pushal();
	CNT(3);
	if (rd8(MAP_AUTOWALK) == 0xff) { /* 0x404497 */
		game_map_walk_step();
		wr8(sp + S_FRAME, rd8(sp + S_FRAME) ^ 1);
		CNT(2);
		return pl_4044a3();
	}
	CNT(1);
	return pump(0x40448b);
}

/* ---- the event check 0x405216 ---- */

static uint32_t ev_405372(void);
static uint32_t ev_40530b(void);

/* 0x4052f1 */
static uint32_t ev_4052f1(void)
{
	set16(EAX, cpu.r[EBX]);
	CNT(3);
	if (rd16(MAP_LAST_CLICK) == AX16)
		return ev_40530b();
	wr16(MAP_LAST_CLICK, AX16);
	CNT(2);
	return ev_405372();
}

/* 0x40530b: the area under sprite 0 (0x404f8c) */
static uint32_t ev_40530b(void)
{
	set16(EAX, 0);
	wr16(MAP_ARGS, 0);
	CNT(2);
	game_map_area_trigger();
	CNT(3);
	if (AX16 == 0xffff) {
		wr16(MAP_LAST_AREA, 0xffff);
		CNT(2);
		return ev_405372();
	}
	CNT(2);
	if (rd16(MAP_LAST_AREA) != AX16) {
		wr16(MAP_LAST_AREA, AX16);
		set16(EAX, AX16 + 0x32);
		CNT(3);
		return ev_405372();
	}
	/* 0x405340 */
	cpu.r[EDI] = rd16(MAP_SEL);
	cpu.r[ESI] = rd16(SEG1 + cpu.r[EDI] + M_SPRITES);
	set16(EAX, rd8(SEG1 + cpu.r[ESI] + S_FRAME) >> 1);
	CNT(7);
	if (AX16 == rd16(MAP_LAST_SPRITE)) {
		set16(EAX, 0xffff);
		CNT(1);
	} else {
		set16(EAX, rd16(MAP_LAST_AREA) + 0x32);
		CNT(3);
	}
	return ev_405372();
}

/* 0x405372: remember the player's facing; EAX = the event (0xffff: none);
 * else wait for the button to go up (0x405399) */
static uint32_t ev_405372(void)
{
	uint32_t saved = cpu.r[EAX];
	push32(saved);
	cpu.r[EDI] = rd16(MAP_SEL);
	cpu.r[ESI] = rd16(SEG1 + cpu.r[EDI] + M_SPRITES);
	set16(EAX, rd8(SEG1 + cpu.r[ESI] + S_FRAME) >> 1);
	wr16(MAP_LAST_SPRITE, AX16);
	cpu.r[EAX] = pop32();
	CNT(10);
	if (AX16 == 0xffff) {
		CNT(1);
		return go(pop32());
	}
	push32(cpu.r[EAX]);
	CNT(2);
	return pump(0x40539f);
}

/* 0x40539f: until the button is up */
uint32_t game_ev_released(void)
{
	cpu.r[EAX] = pop32();
	set8l(EBX, cpu.r[EBX] & 1);
	CNT(3);
	if (cpu.r[EBX] & 1) {
		X86_POLL(0x4053a3);
		push32(cpu.r[EAX]);
		CNT(2);
		return pump(0x40539f);
	}
	CNT(1);
	return go(pop32());
}

/* 0x4052e1 / 0x4052ed: the click is on the player / off it (pop the saved
 * EAX) */
static uint32_t ev_4052e1(void)
{
	cpu.r[EAX] = pop32();
	wr16(MAP_LAST_CLICK, AX16);
	CNT(3);
	return ev_405372();
}

static uint32_t ev_4052ed(void)
{
	cpu.r[EAX] = pop32();
	set16(EBX, AX16);
	CNT(2);
	return ev_4052f1();
}

/* 0x405273: after the second pump (button down at the contact): is the
 * mouse on the player's cell? */
uint32_t game_ev_pumped_b(void)
{
	uint32_t ebx = rd16(MAP_SEL), m = SEG1 + ebx;
	cpu.r[EBX] = ebx;
	cpu.r[ESI] = rd16(m + M_SPRITES);
	uint32_t sp = SEG1 + cpu.r[ESI];
	uint32_t ax = (rol16(rd16(sp + S_X) + rd16(m + M_SCREEN_X) - rd16(m + M_VIEW_X), 4) - 0x30) & 0xffff;
	set16(EAX, ax);
	CNT(9);
	int off = CX16 < ax;
	if (!off) {
		ax = (ax + 0x20 + 0x60) & 0xffff;
		set16(EAX, ax);
		CNT(4);
		off = ax < CX16;
		if (!off) {
			CNT(1);
			return ev_4052e1();
		}
	}
	ax = (rol16(rd16(sp + S_Y) - rd16(m + M_VIEW_Y), 4) + rd16(m + M_SCREEN_Y) - 0x30) & 0xffff;
	set16(EAX, ax);
	CNT(7);
	if (DX16 < ax)
		return ev_4052ed();
	ax = (ax + 0x20 + 0x60) & 0xffff;
	set16(EAX, ax);
	CNT(4);
	if (ax < DX16)
		return ev_4052ed();
	return ev_4052e1();
}

/* 0x405233: after the first pump of the check */
uint32_t game_ev_pumped_a(void)
{
	cpu.r[EAX] = pop32();
	CNT(3);
	if (!(cpu.r[EBX] & 1)) {
		set16(EBX, AX16);
		set8h(EAX, rd8(INPUT_BITS));
		CNT(4);
		if (((cpu.r[EAX] >> 8) & 0xff) == 0)
			return ev_4052f1();
		set16(EAX, cpu.r[EBX]);
		CNT(3);
		if (rd8(MAP_MOVED) != 0xff)
			return ev_4052f1();
		CNT(1);
		wr16(MAP_LAST_CLICK, AX16); /* 0x4052e2 */
		CNT(2);
		return ev_405372();
	}
	CNT(2);
	if (rd8(MAP_MOVED) != 0xff)
		return ev_4052f1();
	push32(cpu.r[EAX]);
	CNT(2);
	return pump(0x405273);
}

/* 0x405216: collision test of sprite 0 (0x40500e); if it touches something,
 * pump and look at the mouse; then the area trigger */
static uint32_t ev_begin(void)
{
	cpu.r[EAX] = 0;
	wr16(MAP_ARGS, 0);
	CNT(2);
	game_map_collision();
	CNT(3);
	if (AX16 == 0xffff) {
		wr16(MAP_LAST_CLICK, AX16); /* 0x405305 */
		CNT(1);
		return ev_40530b();
	}
	push32(cpu.r[EAX]);
	CNT(2);
	return pump(0x405233);
}

/* ---- sub-op 12: one frame (0x4051c2) ---- */

/* 0x4051e3: pop EAX, left button down inside the box 0xf8..0x188 x 0x120..0x140
 * (the screen's button) holds the frame; else the event check */
uint32_t game_s12_pumped_a(void)
{
	cpu.r[EAX] = pop32();
	set8l(EBX, cpu.r[EBX] & 1);
	CNT(3);
	if (!(cpu.r[EBX] & 1))
		return ev_begin();
	CNT(2);
	if (CX16 < 0xf8)
		return ev_begin();
	CNT(2);
	if (CX16 >= 0x188)
		return ev_begin();
	CNT(2);
	if (DX16 < 0x120)
		return ev_begin();
	CNT(2);
	if (DX16 >= 0x140)
		return ev_begin();
	set16(EAX, 0xfffe);
	push32(cpu.r[EAX]);
	CNT(3);
	return pump(0x40520f);
}

/* 0x40520f: wait for the button to go up, then return */
uint32_t game_s12_released(void)
{
	cpu.r[EAX] = pop32();
	set8l(EBX, cpu.r[EBX] & 1);
	CNT(3);
	if (cpu.r[EBX] & 1) {
		X86_POLL(0x405213);
		push32(cpu.r[EAX]);
		CNT(2);
		return pump(0x40520f);
	}
	CNT(1);
	return go(pop32());
}

/* ---- the sprite loop 0x4042f7 and the sub-ops ---- */

/* 0x404305: for each sprite record up to the 0xff flag byte */
static uint32_t sprites_loop(void)
{
	for (;;) {
		CNT(2);
		if (rd8(SEG1 + cpu.r[ESI]) == 0xff) {
			CNT(1);
			return go(pop32());
		}
		uint32_t esi = cpu.r[ESI];
		push32(esi);
		push32(0x404314);
		CNT(2);
		if (game_map_sprite_step())
			return player_begin();
		cpu.r[ESP] += 4; /* ret */
		cpu.r[ESI] = pop32() + SPRITE_SIZE;
		CNT(3);
		X86_POLL(0x404318);
	}
}

static uint32_t sprites_begin(void)
{
	cpu.r[EDI] = rd16(MAP_SEL);
	cpu.r[ESI] = rd16(SEG1 + cpu.r[EDI] + M_SPRITES);
	CNT(2);
	return sprites_loop();
}

/* 0x4051c2: sub-op 12 */
static uint32_t s12_begin(void)
{
	wr8(MAP_MOVED, 0);
	push32(0x4051ce);
	CNT(2);
	return sprites_begin();
}

/* 0x4051ce: after the sprites, redraw the cells that changed (sub-ops 2, 3,
 * 4), then pump */
static uint32_t s12_drawn(void)
{
	game_map_fill_grid();
	game_map_overlay_sprites();
	game_map_redraw_changed();
	push32(cpu.r[EAX]);
	CNT(5);
	return pump(0x4051e3);
}

/* ---- going back out through the native return addresses ---- */

/* `ret`: the instruction was counted; ra is where the chain goes on. The
 * native return addresses are the C functions above; anything else is a
 * unit (the handler's tail 0x40740f). */
static uint32_t go(uint32_t ra)
{
	switch (ra) {
	case 0x403a86: /* the ret of 0x403a70 */
	case 0x40440f: /* the ret of 0x40431b */
		CNT(1);
		return go(pop32());
	case 0x404314: /* pop esi, add esi, 0x10, jmp 0x404305 */
		cpu.r[ESI] = pop32() + SPRITE_SIZE;
		CNT(3);
		X86_POLL(0x404318);
		return sprites_loop();
	case 0x4051ce:
		return s12_drawn();
	case 0x403ed3: /* sub-op 5 after its event check */
		game_map_redraw_all_end();
		return go(pop32());
	case 0x404495: /* jmp 0x4044a3 */
		CNT(1);
		return pl_4044a3();
	case TAIL: /* pop esi */
		cpu.r[ESI] = pop32();
		CNT(1);
		return TAIL_PIECE;
	default:
		return ra;
	}
}

/* ---- the handler 0x4073ee and its tail ---- */

/* 04 26 n, args...: map sub-op n through 0x403a70 (ESI = the argument
 * values; EAX = n), S = AX after it. Sub-ops 4 and 5 first reset the
 * animation slots (0x406dff). The handler pushes ESI and calls 0x403a70;
 * sub-ops that do not pump run in game_map_dispatch and leave for the tail
 * 0x40740f with only the pushed ESI on the stack. */
uint32_t game_op_map(void)
{
	game_parse_args_regs();
	CNT(1);
	uint32_t a = cpu.r[EAX];
	CNT(2);
	if (a == 5 || a == 4) {
		push32(a);
		game_anim_reset();
		cpu.r[EAX] = pop32();
		CNT(2);
	}
	uint32_t esi = cpu.r[ESI];
	cpu.r[ESI] = VM_ARG_VALUES;
	CNT(2);
	if (game_map_op_in_c(a)) {
		game_map_dispatch();
		cpu.r[ESI] = esi; /* the pop of the pushed ESI, at the tail */
		CNT(1);
		return TAIL_PIECE;
	}
	push32(esi);
	if (a != 5 && a != 7 && a != 12)
		x86_trap(0x403a7f, "04 26: sub-op outside the dispatch table");
	push32(TAIL); /* call 0x403a70 */
	game_map_dispatch_args();
	push32(0x403a86); /* call [0x435576 + 4 * eax] */
	switch (a) {
	case 5: /* 0x403e30 */
		game_map_redraw_all_begin();
		push32(0x403ed3);
		CNT(1);
		return ev_begin();
	case 7: /* 0x4042f7 */
		return sprites_begin();
	default: /* 0x4051c2 */
		return s12_begin();
	}
}

/* 0x407410 (after the pop of ESI at 0x40740f): S = AX, on with the script */
uint32_t game_op_map_done(void)
{
	vm_set_reg(VM_REG_S, cpu.r[EAX]);
	CNT(1);
	X86_POLL(0x407416);
	return VM_LOOP;
}

/* Where the oracle of a piece inside the chain ends: the handler's tail
 * 0x407410. Words from ESP up to and including the 0x40740f frame and the
 * ESI under it (read without the trace, which is on). */
int game_chain_words(void)
{
	for (uint32_t i = 0; i < 512; i++) {
		uint32_t v;
		memcpy(&v, x86_ptr(cpu.r[ESP] + 4 * i, 4), 4);
		if (v == TAIL)
			return (int)i + 2;
	}
	x86_trap(cpu.r[ESP], "map chain: no handler frame on the stack");
}
