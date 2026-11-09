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
	/* Stage 5B (src/game/screen.c): the units that native code still calls */
	CALL_SYS(0x401190, "surface_lock", game_lock_regs, 0),
	CALL_SYS(0x4011f0, "cursor_restore", game_cursor_restore_regs, 0),
	CALL_SYS(0x4012b0, "surface_unlock", game_unlock_regs, 0),
	CALL_SYS(0x401900, "text_draw_char", game_draw_char_regs,
		REG_BIT(EAX) | REG_BIT(ECX) | REG_BIT(EDX)),
	/* Stage 6: the dissolve wipe, pieces around its message pump (wipe.c) */
	PIECE(0x401a50, "wipe", game_wipe, 0, 0, 0, 1, NULL),
	PIECE(0x401b90, "wipe_pumped", game_wipe_pumped, 0, 0x12, 0, 1, NULL),
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
	PIECE(0x4060f9, "vm_loop", game_vm_loop, 0, 0, 0, 0, NULL),
	PIECE(0x4060fe, "vm_pumped", game_vm_pumped, AT_FETCH, 0, 0, 1, NULL),
	PIECE(0x406124, "vm_click_tested0", game_vm_click_tested0, REG_BIT(EBX), 0, 0, 1, NULL),
	PIECE(0x406134, "vm_click_tested1", game_vm_click_tested1, REG_BIT(EBX), 0, 0, 1, NULL),
	PIECE(0x40614a, "vm_click_up0", game_vm_click_up0, REG_BIT(EBX), 0, 0, 1, NULL),
	PIECE(0x406153, "vm_click_up1", game_vm_click_up1, REG_BIT(EBX), 0, 0, 1, NULL),
	PIECE(0x406164, "vm_click_done", game_vm_click_done, AT_LOOP, 1, 0, 0, NULL),
	PIECE(0x4061c2, "vm_block_done", game_vm_block_done, AT_LOOP, 1, 0, 0, NULL),
	PIECE(0x4061cd, "vm_run_block", game_vm_run_block, AT_LOOP, 0, 0, 0, NULL),
	PIECE(0x4061d3, "vm_block_return", game_vm_block_return, AFTER_BLOCK, 0, 0, 0, NULL),
	CALL(0x4061d4, "vm_skip_block", game_skip_block_regs, 0, 0),
	PIECE(0x406315, "vm_string_next", game_vm_string_next, AT_FETCH, 0, 0, 1, NULL),
	PIECE(0x406383, "vm_big5_next", game_vm_big5_next, AT_LOOP, 0, 0, 0, NULL),
	CALL(0x406395, "vm_init", game_vm_init_regs, REG_BIT(EAX), 0),
	CALL(0x4063ee, "text_advance", game_text_advance, 0, 0),
	JUMP(0x406491, "op_0a_set_flags", game_op_set_flags, AT_LOOP),
	JUMP(0x4064b5, "op_0b_set_flags_expr", game_op_set_flags_expr, AT_LOOP),
	JUMP(0x4064d9, "op_0c_set_reg", game_op_set_reg, AT_LOOP),
	JUMP(0x4064f8, "op_0d_store_words", game_op_store_words, AT_LOOP),
	JUMP(0x406557, "op_0e_store_bytes", game_op_store_bytes, AT_LOOP),
	JUMP(0x40659e, "op_0f_if", game_op_if, AT_LOOP),
	JUMP(0x4065ae, "op_0f_if_then", game_op_if_then, AFTER_BLOCK),
	JUMP(0x4065ed, "op_0f_if_else", game_op_if_else, AFTER_BLOCK),
	JUMP(0x4065f6, "op_11_wait", game_op_wait, AT_LOOP),
	JUMP_SYS(0x4065fb, "op_11_wait_clear", game_op_wait_clear, NULL),
	JUMP(0x40660c, "op_0410_while", game_op_while, AT_LOOP),
	RESUME(0x40661e, "op_0410_while_next", game_op_while_next, AT_LOOP, 1),
	JUMP(0x406638, "op_0411_continue", game_op_continue, 0),
	JUMP(0x40663b, "op_0412_break", game_op_break, 0),
	JUMP(0x40663e, "op_12_set_label", game_op_set_label, AT_LOOP),
	JUMP(0x406658, "op_13_call_label", game_op_call_label, AT_LOOP),
	RESUME(0x40666b, "op_13_call_label_done", game_op_call_label_done, AT_LOOP, 1),
	JUMP(0x406671, "op_13ext_menu", game_op_menu, AT_LOOP),
	RESUME(0x4066b8, "op_13ext_menu_round", game_op_menu_round, AFTER_BLOCK, 1),
	RESUME_SYS(0x4066bd, "op_13ext_menu_pumped", game_op_menu_pumped, AFTER_BLOCK, 1),
	RESUME(0x4066e6, "op_13ext_menu_framed", game_op_menu_framed, AFTER_BLOCK, 1),
	RESUME(0x4066f6, "op_13ext_menu_tested0", game_op_menu_tested0, AFTER_BLOCK, 1),
	RESUME(0x406704, "op_13ext_menu_handled0", game_op_menu_handled0, AFTER_BLOCK, 1),
	RESUME(0x406714, "op_13ext_menu_waited0", game_op_menu_waited0, AFTER_BLOCK, 1),
	RESUME(0x406728, "op_13ext_menu_tested1", game_op_menu_tested1, AFTER_BLOCK, 1),
	RESUME(0x406736, "op_13ext_menu_handled1", game_op_menu_handled1, AFTER_BLOCK, 1),
	RESUME(0x40674a, "op_13ext_menu_waited1", game_op_menu_waited1, AFTER_BLOCK, 1),
	RESUME(0x406761, "op_13ext_menu_tested2", game_op_menu_tested2, AFTER_BLOCK, 1),
	RESUME(0x40676f, "op_13ext_menu_handled2", game_op_menu_handled2, AFTER_BLOCK, 1),
	RESUME(0x406783, "op_13ext_menu_waited2", game_op_menu_waited2, AFTER_BLOCK, 1),
	RESUME(0x40679a, "op_13ext_menu_tested3", game_op_menu_tested3, AFTER_BLOCK, 1),
	RESUME(0x4067a8, "op_13ext_menu_handled3", game_op_menu_handled3, AFTER_BLOCK, 1),
	RESUME(0x4067bc, "op_13ext_menu_waited3", game_op_menu_waited3, AFTER_BLOCK, 1),
	RESUME(0x4067d3, "op_13ext_menu_tested4", game_op_menu_tested4, AFTER_BLOCK, 1),
	RESUME(0x4067e1, "op_13ext_menu_handled4", game_op_menu_handled4, AFTER_BLOCK, 1),
	RESUME(0x4067f5, "op_13ext_menu_waited4", game_op_menu_waited4, AFTER_BLOCK, 1),
	RESUME(0x406810, "op_13ext_menu_tested5", game_op_menu_tested5, AFTER_BLOCK, 1),
	RESUME_SYS(0x406822, "op_13ext_menu_handled5", game_op_menu_handled5, AFTER_BLOCK, 1),
	RESUME_SYS(0x40683b, "op_13ext_menu_waited5", game_op_menu_waited5, AFTER_BLOCK, 1),
	JUMP_SYS(0x406842, "op_0414_menu_items", game_op_menu_items, NULL),
	CALL(0x4068e1, "vm_menu_item", game_menu_item_regs, REG_BIT(EDX), 0),
	JUMP_SYS(0x406916, "op_0415_cursor", game_op_cursor, NULL),
	JUMP(0x4069be, "op_18_text_colour", game_op_text_colour, AT_LOOP),
	JUMP(0x4069ce, "op_15_print_number", game_op_print_number, AT_LOOP),
	RESUME(0x4069f7, "op_15_print_number_done", game_op_print_number_done, AT_LOOP, 1),
	CALL(0x4069fd, "vm_format_number", game_format_number_regs, 0, 0),
	JUMP(0x406a9a, "op_14_call_offset", game_op_call_offset, AT_LOOP),
	RESUME(0x406aa7, "op_14_call_offset_done", game_op_call_offset_done, AT_LOOP, 1),
	/* ECX (the frame count) goes into 0x4075c6 */
	JUMP(0x406aad, "op_16_delay", game_op_delay, AT_LOOP & ~REG_BIT(ECX)),
	RESUME(0x406abd, "op_16_delay_done", game_op_delay_done, AT_LOOP, 0),
	RESUME(0x406ac7, "op_16_delay_click_done", game_op_delay_click_done, AT_LOOP, 0),
	PIECE(0x406acc, "wait_click", game_wait_click, 0, 0, 0, 0, NULL),
	PIECE(0x406adf, "wait_click_framed", game_wait_click_framed, 0, 0, 0, 0, NULL),
	PIECE(0x406ae6, "wait_click_tested", game_wait_click_tested, 0, 0, 0, 0, NULL),
	PIECE(0x406afb, "wait_click_release_framed", game_wait_click_release_framed, 0, 0, 0, 0, NULL),
	PIECE(0x406b02, "wait_click_release_tested", game_wait_click_release_tested, 0, 0, 0, 0, NULL),
	JUMP_SYS(0x406b05, "op_17_clear_text", game_op_clear_text, NULL),
	CALL_SYS(0x406b24, "text_clear_window", game_clear_text_window, 0),
	JUMP_SYS(0x406b76, "op_0417_fill_rect", game_op_fill_rect, NULL),
	JUMP_SYS(0x406bcb, "op_0418_xor_rect", game_op_xor_rect, NULL),
	JUMP_SYS(0x406c20, "op_0419_copy_rect", game_op_copy_rect, NULL),
	JUMP_SYS(0x406c85, "op_041a_swap_rect", game_op_swap_rect, NULL),
	JUMP_SYS(0x406cea, "op_041b_masked_rect", game_op_masked_rect, NULL),
	JUMP_SYS(0x406d8b, "op_041c_load_file", game_op_load_file, NULL),
	CALL(0x406dc4, "vm_string_arg", game_string_arg_regs, 0, 0),
	JUMP_SYS(0x406e11, "op_041d_picture", game_op_picture, NULL),
	JUMP_SYS(0x406f06, "op_0416_palette", game_op_palette, NULL),
	SPIN(0x406f41, "palette_wait", game_palette_wait_piece, AT_LOOP),
	JUMP_SYS(0x406f7c, "op_041e_jump_mes", game_op_jump_mes, NULL),
	JUMP_SYS(0x406fcd, "op_041f_call_mes", game_op_call_mes, NULL),
	/* ECX's upper half is the count's (`mov cx, 0x1e`) */
	RESUME_SYS(0x40704a, "op_041f_call_mes_done", game_op_call_mes_done,
		REG_BIT(EDX) | REG_BIT(EBX) | REG_BIT(EDI), 4),
	JUMP_SYS_R(0x407096, "op_1a_anim", game_op_anim, NULL, REG_BIT(EDI)),
	RESUME(0x4070b6, "op_1a_anim_done", game_op_anim_done, AT_LOOP, 1),
	JUMP(0x4070bc, "op_0420_click_handler", game_op_click_handler, AT_LOOP),
	JUMP_SYS(0x4070d5, "op_0421_flag_file", game_op_flag_file, NULL),
	JUMP(0x4071a4, "op_0423_hit_test", game_op_hit_test, AT_LOOP),
	JUMP_SYS(0x407204, "op_19_misc", game_op_misc, NULL),
	/* 3 words: the saved ESI, 2 and 0 pushed before the wipe's call */
	RESUME_SYS(0x40738b, "op_19_wipe_done", game_op_wipe_done, AT_LOOP, 3),
	JUMP(0x40739b, "op_0422_args_only", game_op_args_only, AT_LOOP),
	JUMP_SYS(0x4073a5, "op_0424_music", game_op_music, NULL),
	JUMP_SYS(0x4073cd, "op_0425_voice", game_op_voice, NULL),
	/* Stage 6 (src/game/map_pump.c): the handler runs the chain down to the first
	 * pump, the pump sites' return addresses are the pieces. At the handler's tail
	 * only AX (S) is live; at a pump every register is. */
	{ 0x4073ee, "op_0426_map", NULL, game_op_map, 0 },
	RESUME(0x407410, "op_0426_map_done", game_op_map_done, AT_LOOP, 0),
	CALL(0x40741b, "vm_parse_args", game_parse_args_regs, REG_BIT(EDX), 0),
	PIECE(0x4074d5, "input_test", game_input_test, 0, 0, 0, 0, NULL),
	PIECE(0x4074db, "input_tested", game_input_tested, 0, 1, 1, 0, NULL),
	PIECE(0x4075c6, "wait_frames", game_wait_frames, 0, 0, 0, 0, NULL),
	PIECE(0x4075d7, "wait_frames_pumped", game_wait_frames_pumped, 0, 1, 0, 1, NULL),
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
