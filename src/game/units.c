#include "game/game.h"

#include <stddef.h>

#include "game/expr.h"
#include "game/file.h"
#include "game/map.h"
#include "game/startup.h"
#include "game/vm.h"
#include "x86/x86.h"

/* The table of the units: what the interpreter enters by native address.
 * CALL: a called function (hle_call, `run`, returns to the caller's return
 * address); JUMP: a piece or opcode handler (hle_jump, returns the pc to go
 * on at); SPIN: a piece of a wait loop that returns its own address while it
 * waits (the timer is polled for each such return, like the native backward
 * jump). The register contracts are documented at the functions. */
#define CALL(addr, name, fn, scratch, flags) { addr, name, fn, NULL, 0 }
#define JUMP(addr, name, fn, scratch) { addr, name, NULL, fn, 0 }
#define RESUME(addr, name, fn, scratch, words) { addr, name, NULL, fn, 0 }
#define JUMP_SYS(addr, name, fn, native) { addr, name, NULL, fn, 0 }
#define CALL_AL(addr, name, fn, scratch) { addr, name, fn, NULL, 0 }
#define JUMP_SYS_R(addr, name, fn, native, scratch) { addr, name, NULL, fn, 0 }
#define RESUME_SYS(addr, name, fn, scratch, words) { addr, name, NULL, fn, 0 }
#define CALL_SYS(addr, name, fn, scratch) { addr, name, fn, NULL, 0 }
#define PIECE(addr, name, fn, scratch, words, flags, sys, native) { addr, name, NULL, fn, 0 }
#define SPIN(addr, name, fn, scratch) { addr, name, NULL, fn, 1 }
#define CHAIN(addr, name, fn) { addr, name, NULL, fn, 0 }
#define FILE_SCRATCH 0
#define AT_LOOP 0
#define AFTER_BLOCK 0
#define AT_FETCH 0
#define MAP_MOVER_SCRATCH 0
#define MAP_TAIL_SCRATCH 0

/* Sorted by address. Keep in step with HLE_GAME_OVERRIDE (CALL) and
 * HLE_GAME_JUMP (JUMP) in src/hle/overrides.h; the contracts are documented
 * at each function. */
const struct game_unit game_units[] = {
	/* Stage 5B startup/input/file */
	CALL_SYS(0x401000, "file_open", game_file_open_regs, FILE_SCRATCH),
	CALL_SYS(0x401050, "file_load", game_file_load_regs, FILE_SCRATCH),
	CALL_SYS(0x401160, "file_save", game_file_save_regs, FILE_SCRATCH),
	/* Stage 5B (src/game/screen.c): the units that native code still calls */
	CALL_SYS(0x401190, "surface_lock", game_lock_regs, 0),
	CALL_SYS(0x4011f0, "cursor_restore", game_cursor_restore_regs, 0),
	CALL_SYS(0x4012b0, "surface_unlock", game_unlock_regs, 0),
	CALL_SYS(0x401900, "text_draw_char", game_draw_char_regs,
		REG_BIT(EAX) | REG_BIT(ECX) | REG_BIT(EDX)),
	/* Stage 6: the dissolve wipe, pieces around its message pump (wipe.c) */
	PIECE(0x401a50, "wipe", game_wipe, 0, 0, 0, 1, NULL),
	PIECE(0x401b90, "wipe_pumped", game_wipe_pumped, 0, 0x12, 0, 1, NULL),
	/* Stage 5B startup/input/file; WinMain's pieces run uncompared (game.c) */
	PIECE(0x402090, "winmain", game_winmain, REG_BIT(ECX) | REG_BIT(EDX), 0, 0, 1, NULL),
	PIECE(0x4021a3, "winmain_done", game_winmain_done, REG_BIT(ECX) | REG_BIT(EDX), 11, 0, 1, NULL),
	CALL_SYS(0x4021c0, "wndproc", game_wndproc, REG_BIT(ECX) | REG_BIT(EDX)),
	PIECE(0x403510, "anim_control", game_anim_control_piece, 0, 0, 0, 1, NULL),
	SPIN(0x4035b5, "anim_wait", game_anim_wait_piece, 0),
	CALL_SYS(0x403624, "anim_step", game_anim_step, 0),
	CALL_SYS(0x403661, "anim_run_slot", game_anim_run_slot, 0),
	CALL(0x4037fc, "script_eval", game_eval_regs, REG_BIT(EBX) | REG_BIT(EDX), 0),
	CALL(0x4039f7, "script_read_constant", game_read_constant_regs, 0, 0),
	/* Stage 5A (src/game/map*.c) */
	CALL(0x403b28, "map_fill_grid", game_map_fill_grid,
		REG_BIT(EAX) | REG_BIT(EBX) | REG_BIT(ESI) | REG_BIT(EDI), 0),
	CALL(0x403bd2, "map_overlay_sprites", game_map_overlay_sprites,
		REG_BIT(EAX) | REG_BIT(EDX) | REG_BIT(EBX) | REG_BIT(EDI), 0),
	CALL_SYS(0x403d45, "map_redraw_changed", game_map_redraw_changed,
		REG_BIT(EAX) | REG_BIT(ESI) | REG_BIT(EDI)),
	CALL(0x403eff, "map_cell_draw", game_map_cell_draw, 0, 0),
	CALL(0x4040f0, "map_cell_clear_bit3", game_map_cell_clear_bit3, 0, 0),
	CHAIN(0x40448b, "map_player_pumped_a", game_pl_pumped_a),
	CHAIN(0x4044a9, "map_player_pumped_b", game_pl_pumped_b),
	CHAIN(0x4044b7, "map_player_click", game_pl_click),
	CHAIN(0x4045eb, "map_player_keys", game_pl_keys),
	CALL_AL(0x404731, "map_move_up", game_map_move_up, MAP_MOVER_SCRATCH),
	CALL_AL(0x404937, "map_move_down", game_map_move_down, MAP_MOVER_SCRATCH),
	CALL_AL(0x404a9a, "map_move_left", game_map_move_left, MAP_MOVER_SCRATCH),
	CALL_AL(0x404c9f, "map_move_right", game_map_move_right, MAP_MOVER_SCRATCH),
	CALL(0x404f8c, "map_area_trigger", game_map_area_trigger,
		REG_BIT(ECX) | REG_BIT(EDX) | REG_BIT(ESI) | REG_BIT(EDI), 0),
	CALL(0x40500e, "map_collision", game_map_collision, REG_BIT(ESI) | REG_BIT(EDI), 0),
	CHAIN(0x4051e3, "map_frame_pumped", game_s12_pumped_a),
	CHAIN(0x40520f, "map_frame_released", game_s12_released),
	CHAIN(0x405233, "map_event_pumped_a", game_ev_pumped_a),
	CHAIN(0x405273, "map_event_pumped_b", game_ev_pumped_b),
	CHAIN(0x40539f, "map_event_released", game_ev_released),
	CHAIN(0x40559d, "map_rclick_pumped", game_rc_pumped),
	CALL(0x405691, "map_walk_step", game_map_walk_step,
		REG_BIT(EAX) | REG_BIT(ECX) | REG_BIT(EDX) | REG_BIT(EBX), 0),
	CALL(0x405966, "map_plan_walk", game_map_plan_walk,
		REG_BIT(EAX) | REG_BIT(ECX) | REG_BIT(EDX) | REG_BIT(EBX) | REG_BIT(EDI), 0),
	/* Stage 6: the map's pump (0x40609c: pushad and the pump; 0x4060a2: its result) */
	{ 0x40609c, "map_pump", NULL, game_pump, 0 },
	{ 0x4060a2, "map_pump_done", NULL, game_pump_done, 0 },
	PIECE(0x4060d4, "vm_entry", game_vm_entry, AT_LOOP, -1, 0, 1, NULL),
	CALL(0x4063ee, "text_advance", game_text_advance, 0, 0),
	CALL(0x406455, "script_set_flag", game_set_flag_regs, 0, 0),
	JUMP_SYS(0x406842, "op_0414_menu_items", game_op_menu_items, NULL),
	JUMP_SYS(0x406916, "op_0415_cursor", game_op_cursor, NULL),
	JUMP_SYS(0x406b05, "op_17_clear_text", game_op_clear_text, NULL),
	CALL_SYS(0x406b24, "text_clear_window", game_clear_text_window, 0),
	JUMP_SYS(0x406b76, "op_0417_fill_rect", game_op_fill_rect, NULL),
	JUMP_SYS(0x406bcb, "op_0418_xor_rect", game_op_xor_rect, NULL),
	JUMP_SYS(0x406c20, "op_0419_copy_rect", game_op_copy_rect, NULL),
	JUMP_SYS(0x406c85, "op_041a_swap_rect", game_op_swap_rect, NULL),
	JUMP_SYS(0x406cea, "op_041b_masked_rect", game_op_masked_rect, NULL),
	JUMP_SYS(0x406d8b, "op_041c_load_file", game_op_load_file, NULL),
	CALL(0x406dff, "anim_reset", game_anim_reset, 0, 0),
	JUMP_SYS(0x406e11, "op_041d_picture", game_op_picture, NULL),
	JUMP_SYS(0x406f06, "op_0416_palette", game_op_palette, NULL),
	SPIN(0x406f41, "palette_wait", game_palette_wait_piece, AT_LOOP),
	JUMP_SYS_R(0x407096, "op_1a_anim", game_op_anim, NULL, REG_BIT(EDI)),
	RESUME(0x4070b6, "op_1a_anim_done", game_op_anim_done, AT_LOOP, 1),
	JUMP_SYS(0x407204, "op_19_misc", game_op_misc, NULL),
	/* 3 words: the saved ESI, 2 and 0 pushed before the wipe's call */
	RESUME_SYS(0x40738b, "op_19_wipe_done", game_op_wipe_done, AT_LOOP, 3),
	JUMP_SYS(0x4073a5, "op_0424_music", game_op_music, NULL),
	JUMP_SYS(0x4073cd, "op_0425_voice", game_op_voice, NULL),
	/* Stage 6 (src/game/map_pump.c): the handler runs the chain down to the first
	 * pump, the pump sites' return addresses are the pieces. At the handler's tail
	 * only AX (S) is live; at a pump every register is. */
	{ 0x4073ee, "op_0426_map", NULL, game_op_map, 0 },
	RESUME(0x407410, "op_0426_map_done", game_op_map_done, AT_LOOP, 0),
	CALL(0x40752c, "palette_copy", game_palette_copy_regs, 0, 0),
};
const int game_unit_count = (int)(sizeof game_units / sizeof game_units[0]);

const struct game_unit *game_unit_find(uint32_t addr)
{
	int lo = 0, hi = game_unit_count;
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		if (game_units[mid].addr < addr)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < game_unit_count && game_units[lo].addr == addr ? &game_units[lo] : NULL;
}
