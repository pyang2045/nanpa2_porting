#include "game/game.h"

#include <stddef.h>

#include "game/expr.h"
#include "game/file.h"
#include "game/map.h"
#include "game/startup.h"
#include "game/vm.h"
#include "x86/x86.h"

/* Registers dead where an opcode handler goes back to the VM loop 0x4060f9
 * (only ESP, EBP and ESI are live there, tools/liveness.py). */
#define AT_LOOP (REG_BIT(EAX) | REG_BIT(ECX) | REG_BIT(EDX) | REG_BIT(EBX) | REG_BIT(EDI))

/* After a block (0f's pieces): they may `ret` out of the running block
 * with AL, so EAX is live there too. */
#define AFTER_BLOCK (REG_BIT(ECX) | REG_BIT(EDX) | REG_BIT(EBX) | REG_BIT(EDI))

#define CALL(addr, name, fn, scratch, flags) { addr, name, fn, NULL, scratch, flags, NULL, 0, 0, 0, 0 }
#define JUMP(addr, name, fn, scratch) { addr, name, NULL, fn, scratch, 0, NULL, 0, 0, 0, 0 }
/* the rest of a handler after a call exit, with `words` pushed before it */
#define RESUME(addr, name, fn, scratch, words) { addr, name, NULL, fn, scratch, 0, NULL, 0, words, 0, 0 }
/* a handler that calls native routines (src/game/ops_sys.c); native: the
 * cases it leaves to the original handler, or NULL */
#define JUMP_SYS(addr, name, fn, native) { addr, name, NULL, fn, AT_LOOP, 0, native, 1, 0, 0, 0 }

/* a called unit whose result is AL (the rest of EAX is scratch) */
#define CALL_AL(addr, name, fn, scratch) { addr, name, fn, NULL, scratch, 0, NULL, 0, 0, 0xff, 0 }

/* Stage 5B: the same with the scratch registers given (a handler that
 * ends at a call exit leaves the callee's arguments in registers) */
#define JUMP_SYS_R(addr, name, fn, native, scratch) { addr, name, NULL, fn, scratch, 0, native, 1, 0, 0, 0 }

/* a RESUME piece that calls native routines (file loads, 0x403624) */
#define RESUME_SYS(addr, name, fn, scratch, words) { addr, name, NULL, fn, scratch, 0, NULL, 1, words, 0, 0 }

/* Stage 4 (src/game/loop.c, text.c, anim.c). Called units that call
 * native routines, or with cases left to the original (native) */
#define CALL_SYS(addr, name, fn, scratch) { addr, name, fn, NULL, scratch, 0, NULL, 1, 0, 0, 0 }
#define CALL_NATIVE(addr, name, fn, scratch, native) { addr, name, fn, NULL, scratch, 0, native, 0, 0, 0, 0 }
/* a piece of the VM loop or of a wait: the words pushed before it, whether
 * a caller reads the flags it leaves (it ends with a `ret`), whether it
 * calls native routines (text drawing, the animation blits), the cases
 * left to the native code */
#define PIECE(addr, name, fn, scratch, words, flags, sys, native) \
	{ addr, name, NULL, fn, scratch, flags, native, sys, words, 0, 0 }

/* Stage 5B: a wait loop that is a piece returning itself while it waits.
 * game_jump polls for each such return (the native backward jump does), so
 * timer ticks land between iterations, also under NANPA2_DIFF */
#define SPIN(addr, name, fn, scratch) \
	{ addr, name, NULL, fn, scratch, 0, NULL, 1, 0, 0, 1 }

/* Stage 5B startup/input/file: file access. They scratch the C runtime's
 * cdecl registers; EBX, ESI, EDI, EBP are kept */
#define FILE_SCRATCH (REG_BIT(EAX) | REG_BIT(ECX) | REG_BIT(EDX))

/* Where the fetch dispatches to: handlers (EDX reaches the native map
 * code of 04 26, EAX is the opcode index), the end of a block (`ret` with
 * AL, ECX's and EDX's upper halves), the frame wait (ECX) or the input
 * test (AL). EBX and EDI are dead at all of them. */
#define AT_FETCH (REG_BIT(EBX) | REG_BIT(EDI))

/* the movers (src/game/map_move.c): AL is the result, ECX, EDX, EBX are scratch */
#define MAP_MOVER_SCRATCH (REG_BIT(EAX) | REG_BIT(ECX) | REG_BIT(EDX) | REG_BIT(EBX))

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
	CALL_NATIVE(0x40431b, "map_sprite_step", game_map_sprite_step, MAP_MOVER_SCRATCH,
		game_native_sprite_step),
	CALL_AL(0x404731, "map_move_up", game_map_move_up, MAP_MOVER_SCRATCH),
	CALL_AL(0x404937, "map_move_down", game_map_move_down, MAP_MOVER_SCRATCH),
	CALL_AL(0x404a9a, "map_move_left", game_map_move_left, MAP_MOVER_SCRATCH),
	CALL_AL(0x404c9f, "map_move_right", game_map_move_right, MAP_MOVER_SCRATCH),
	CALL(0x404f8c, "map_area_trigger", game_map_area_trigger,
		REG_BIT(ECX) | REG_BIT(EDX) | REG_BIT(ESI) | REG_BIT(EDI), 0),
	CALL(0x40500e, "map_collision", game_map_collision, REG_BIT(ESI) | REG_BIT(EDI), 0),
	CALL(0x405691, "map_walk_step", game_map_walk_step,
		REG_BIT(EAX) | REG_BIT(ECX) | REG_BIT(EDX) | REG_BIT(EBX), 0),
	CALL(0x405966, "map_plan_walk", game_map_plan_walk,
		REG_BIT(EAX) | REG_BIT(ECX) | REG_BIT(EDX) | REG_BIT(EBX) | REG_BIT(EDI), 0),
	PIECE(0x4060d4, "vm_entry", game_vm_entry, AT_LOOP, -1, 0, 1, NULL),
	PIECE(0x4060f9, "vm_loop", game_vm_loop, 0, 0, 0, 0, NULL),
	PIECE(0x4060fe, "vm_pumped", game_vm_pumped, AT_FETCH, 0, 0, 1, NULL),
	PIECE(0x406124, "vm_click_tested0", game_vm_click_tested0, AT_FETCH, 0, 0, 1, game_native_click0),
	PIECE(0x406134, "vm_click_tested1", game_vm_click_tested1, AT_FETCH, 0, 0, 1, game_native_click1),
	PIECE(0x4061c2, "vm_block_done", game_vm_block_done, AT_LOOP, 1, 0, 0, NULL),
	PIECE(0x4061cd, "vm_run_block", game_vm_run_block, AT_LOOP, 0, 0, 0, NULL),
	PIECE(0x4061d3, "vm_block_return", game_vm_block_return, AFTER_BLOCK, 0, 0, 0, NULL),
	CALL(0x4061d4, "vm_skip_block", game_skip_block_regs, 0, 0),
	PIECE(0x406315, "vm_string_next", game_vm_string_next, AT_FETCH, 0, 0, 1, NULL),
	PIECE(0x406383, "vm_big5_next", game_vm_big5_next, AT_LOOP, 0, 0, 0, NULL),
	CALL(0x406395, "vm_init", game_vm_init_regs, REG_BIT(EAX), 0),
	CALL(0x4063ee, "text_advance", game_text_advance, 0, 0),
	CALL(0x406455, "script_set_flag", game_set_flag_regs, 0, 0),
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
	CALL(0x406dff, "anim_reset", game_anim_reset, 0, 0),
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
	/* not enabled in overrides.h yet (no call seen under the diff) */
	JUMP(0x4070bc, "op_0420_click_handler", game_op_click_handler, AT_LOOP),
	JUMP_SYS(0x4070d5, "op_0421_flag_file", game_op_flag_file, NULL),
	JUMP(0x4071a4, "op_0423_hit_test", game_op_hit_test, AT_LOOP),
	JUMP_SYS(0x407204, "op_19_misc", game_op_misc, NULL),
	/* 3 words: the saved ESI, 2 and 0 pushed before the wipe's call */
	RESUME_SYS(0x40738b, "op_19_wipe_done", game_op_wipe_done, AT_LOOP, 3),
	JUMP(0x40739b, "op_0422_args_only", game_op_args_only, AT_LOOP),
	JUMP_SYS(0x4073a5, "op_0424_music", game_op_music, NULL),
	JUMP_SYS(0x4073cd, "op_0425_voice", game_op_voice, NULL),
	JUMP_SYS(0x4073ee, "op_0426_map", game_op_map, game_native_map),
	CALL(0x40741b, "vm_parse_args", game_parse_args_regs, REG_BIT(EDX), 0),
	PIECE(0x4074d5, "input_test", game_input_test, 0, 0, 0, 0, NULL),
	PIECE(0x4074db, "input_tested", game_input_tested, 0, 1, 1, 0, NULL),
	CALL(0x40752c, "palette_copy", game_palette_copy_regs, 0, 0),
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
