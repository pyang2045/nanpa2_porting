/*
 * The town map, part 5 (stage 5A): the sprite scripts, 0x40431b.
 *
 * Every frame the map steps each sprite (sub-op 7, 0x4042f7, native: it is
 * a loop that calls this for each record). A sprite runs a movement script
 * (S_SCRIPT, found through the script pointer table M_SCRIPTS; S_SCRIPT_PC
 * is the position in it): while it has steps left (S_COUNT) it takes one
 * in its movement kind (S_KIND); when it has none it reads the next script
 * byte, whose high nibble is the kind and low nibble the count:
 *   1 up, 2 down, 3 left, 4 right, 5 and 7 stand (5 flips the step frame),
 *   6 the player (mouse and keys: native, it pumps messages),
 *   8 left+up, 9 right+up, 10 left+down, 11 right+down, 13 walk to the
 *   sprite's destination (S_DEST_X, S_DEST_Y) by the walk grid,
 *   0xc n: take the position of history record n (the followers),
 *   0xe n: a random one of the kinds 1-4 for n steps,
 *   0xf n: n < 8 set the frame to n, 8 show, 9 hide,
 *   0xff: back to the start of the script.
 * A count of 0xf never runs out. After a step the frame's step bit flips.
 *
 * A sprite of kind 6 is the player's step (map_pump.c: it pumps messages,
 * so it is pieces joined by call exits): game_map_sprite_step returns 1
 * then, with the stack as the native code has it at 0x404450. The step
 * itself is the movers' (map_move.c) and the planner's (map_path.c).
 */
#include "game/map.h"


/* the walk to the destination: the planner if no route is set, else one
 * step along it (kind 13, 0x4046f3) */
static void walk_to_destination(uint32_t sel, uint32_t sp)
{
	cpu.r[EDI] = sel;
	X86_COUNT(6);
	if (MV(autowalk) == 0xff) {
		X86_COUNT(1); /* call */
		game_map_walk_step();
		X86_COUNT(1); /* jmp 0x404448 */
		return;
	}
	uint32_t saved[8];
	for (int i = 0; i < 8; i++)
		saved[i] = cpu.r[i];
	cpu.r[ECX] = rd8(sp + S_DEST_X);
	cpu.r[EDX] = rd8(sp + S_DEST_Y);
	MV_SET(approach, 1);
	X86_COUNT(5);
	game_map_plan_walk();
	for (int i = 0; i < 8; i++)
		cpu.r[i] = saved[i];
}

/* 0x404410: one step in the sprite's kind (its `call` and the stub's
 * `jmp 0x404448`, xor and ret are counted here) */
static int step(uint32_t sel, uint32_t sp)
{
	uint32_t count = rd8(sp + S_COUNT);
	X86_COUNT(1 + 2 + 3);
	if (count != 0xf) {
		wr8(sp + S_COUNT, count - 1);
		X86_COUNT(1);
	}
	uint32_t kind = rd8(sp + S_KIND);
	switch (kind) {
	case 1: X86_COUNT(2); game_map_move_up(); break;
	case 2: X86_COUNT(2); game_map_move_down(); break;
	case 3: X86_COUNT(2); game_map_move_left(); break;
	case 4: X86_COUNT(2); game_map_move_right(); break;
	case 5: break;
	case 6:
		push32(0x40440f); /* the return address of `call 0x404410`; the player's code goes on at 0x404450 */
		return 1;
	case 7:
		X86_COUNT(1 + 1);
		return 0; /* stands still, the frame stays */
	case 8: X86_COUNT(3); game_map_move_left(); game_map_move_up(); break;
	case 9: X86_COUNT(3); game_map_move_right(); game_map_move_up(); break;
	case 10: X86_COUNT(3); game_map_move_left(); game_map_move_down(); break;
	case 11: X86_COUNT(3); game_map_move_right(); game_map_move_down(); break;
	case 13: walk_to_destination(sel, sp); break;
	default:
		x86_trap(0x404410, "map: a sprite movement kind the C does not run");
	}
	wr8(sp + S_FRAME, rd8(sp + S_FRAME) ^ 1);
	X86_COUNT(2 + 1); /* xor, ret, and the ret of 0x40440f */
	return 0;
}

/* 0x40431b: ESI = the sprite, EDI = the map selector (kept); ECX, EDX, EBX
 * and EAX are scratch. 1 if the step is the player's (kind 6). */
int game_map_sprite_step(void)
{
	uint32_t sel = cpu.r[EDI], sp = SEG1 + cpu.r[ESI], m = SEG1 + sel;
	uint32_t fl = rd8(sp + S_FLAGS);
	X86_COUNT(3);
	if (!(fl & SPRITE_ACTIVE)) {
		X86_COUNT(1);
		return 0;
	}
	X86_COUNT(2);
	if (fl & SPRITE_OFF) {
		X86_COUNT(1);
		return 0;
	}
	X86_COUNT(2);
	if (rd8(sp + S_COUNT) == 0) {
		uint32_t b;
		for (;;) {
			uint32_t t = (rd8(sp + S_SCRIPT) * 2 + rd16(m + M_SCRIPTS)) & 0xffff;
			uint32_t base = rd16(SEG1 + t);
			uint32_t pc = rd8(sp + S_SCRIPT_PC);
			b = rd8(SEG1 + base + pc);
			X86_COUNT(9);
			if (b != 0xff)
				break;
			wr8(sp + S_SCRIPT_PC, 0);
			X86_COUNT(2);
		}
		wr8(sp + S_SCRIPT_PC, (rd8(sp + S_SCRIPT_PC) + 1) & 0xff);
		uint32_t lo = b & 0xf, hi = b >> 4;
		X86_COUNT(7);
		if (hi == 0xf) {
			if (lo < 8) {
				wr8(sp + S_FRAME, lo);
				X86_COUNT(4);
			} else if (lo == 8) {
				wr8(sp + S_FLAGS, rd8(sp + S_FLAGS) | SPRITE_DRAWN);
				X86_COUNT(6);
			} else if (lo == 9) {
				wr8(sp + S_FLAGS, rd8(sp + S_FLAGS) & ~SPRITE_DRAWN);
				X86_COUNT(8);
			} else {
				X86_COUNT(7);
			}
			return 0;
		}
		X86_COUNT(2);
		if (hi == 0xc) {
			uint32_t n = lo * 5;
			wr8(sp + S_FRAME, hist_rd8(n));
			wr32(sp + S_X, hist_rd32(1 + n));
			X86_COUNT(9);
			return 0;
		}
		X86_COUNT(2);
		if (hi == 0xe) {
			uint32_t r = MV(random_seed);
			uint32_t nr = ((((r << 2) | (r >> 7)) & 0xff) + r + 1) & 0xff;
			MV_SET(random_seed, nr);
			hi = (nr & 3) + 1;
			X86_COUNT(9);
		}
		wr8(sp + S_KIND, hi);
		X86_COUNT(4);
		if (lo == 0) {
			lo = 1;
			X86_COUNT(1);
		}
		wr8(sp + S_COUNT, lo);
	}
	return step(sel, sp);
}
