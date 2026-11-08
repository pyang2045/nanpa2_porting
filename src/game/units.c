#include "game/game.h"

#include <stddef.h>

#include "game/expr.h"
#include "game/vm.h"
#include "x86/x86.h"

/* Registers dead where an opcode handler goes back to the VM loop 0x4060f9
 * (only ESP, EBP and ESI are live there, tools/liveness.py). */
#define AT_LOOP (REG_BIT(EAX) | REG_BIT(ECX) | REG_BIT(EDX) | REG_BIT(EBX) | REG_BIT(EDI))

/* After a block (0f's pieces): they may `ret` out of the running block
 * with AL, so EAX is live there too. */
#define AFTER_BLOCK (REG_BIT(ECX) | REG_BIT(EDX) | REG_BIT(EBX) | REG_BIT(EDI))

#define CALL(addr, name, fn, scratch, flags) { addr, name, fn, NULL, scratch, flags, NULL, 0, 0 }
#define JUMP(addr, name, fn, scratch) { addr, name, NULL, fn, scratch, 0, NULL, 0, 0 }
/* the rest of a handler after a call exit, with `words` pushed before it */
#define RESUME(addr, name, fn, scratch, words) { addr, name, NULL, fn, scratch, 0, NULL, 0, words }
/* a handler that calls native routines (src/game/ops_sys.c); native: the
 * cases it leaves to the original handler, or NULL */
#define JUMP_SYS(addr, name, fn, native) { addr, name, NULL, fn, AT_LOOP, 0, native, 1, 0 }

/* Sorted by address. Keep in step with HLE_GAME_OVERRIDE (CALL) and
 * HLE_GAME_JUMP (JUMP) in src/hle/overrides.h; the contracts are documented
 * at each function. */
const struct game_unit game_units[] = {
	CALL(0x4037fc, "script_eval", game_eval_regs, REG_BIT(EBX) | REG_BIT(EDX), 0),
	CALL(0x4039f7, "script_read_constant", game_read_constant_regs, 0, 0),
	CALL(0x4061d4, "vm_skip_block", game_skip_block_regs, 0, 0),
	CALL(0x406395, "vm_init", game_vm_init_regs, REG_BIT(EAX), 0),
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
	JUMP_SYS(0x406842, "op_0414_menu_items", game_op_menu_items, NULL),
	CALL(0x4068e1, "vm_menu_item", game_menu_item_regs, REG_BIT(EDX), 0),
	JUMP_SYS(0x406916, "op_0415_cursor", game_op_cursor, NULL),
	JUMP(0x4069be, "op_18_text_colour", game_op_text_colour, AT_LOOP),
	JUMP(0x406a9a, "op_14_call_offset", game_op_call_offset, AT_LOOP),
	RESUME(0x406aa7, "op_14_call_offset_done", game_op_call_offset_done, AT_LOOP, 1),
	/* ECX (the frame count) goes into 0x4075c6 */
	JUMP(0x406aad, "op_16_delay", game_op_delay, AT_LOOP & ~REG_BIT(ECX)),
	JUMP_SYS(0x406b05, "op_17_clear_text", game_op_clear_text, NULL),
	JUMP_SYS(0x406b76, "op_0417_fill_rect", game_op_fill_rect, NULL),
	JUMP_SYS(0x406bcb, "op_0418_xor_rect", game_op_xor_rect, NULL),
	JUMP_SYS(0x406c20, "op_0419_copy_rect", game_op_copy_rect, NULL),
	JUMP_SYS(0x406c85, "op_041a_swap_rect", game_op_swap_rect, NULL),
	JUMP_SYS(0x406cea, "op_041b_masked_rect", game_op_masked_rect, NULL),
	JUMP_SYS(0x406d8b, "op_041c_load_file", game_op_load_file, NULL),
	CALL(0x406dc4, "vm_string_arg", game_string_arg_regs, 0, 0),
	JUMP_SYS(0x406e11, "op_041d_picture", game_op_picture, NULL),
	JUMP_SYS(0x406f06, "op_0416_palette", game_op_palette, game_native_palette),
	JUMP_SYS(0x407096, "op_1a_anim", game_op_anim, game_native_anim),
	/* not enabled in overrides.h yet (no call seen under the diff) */
	JUMP(0x4070bc, "op_0420_click_handler", game_op_click_handler, AT_LOOP),
	JUMP(0x4071a4, "op_0423_hit_test", game_op_hit_test, AT_LOOP),
	JUMP_SYS(0x407204, "op_19_misc", game_op_misc, game_native_misc),
	JUMP(0x40739b, "op_0422_args_only", game_op_args_only, AT_LOOP),
	JUMP_SYS(0x4073a5, "op_0424_music", game_op_music, NULL),
	JUMP_SYS(0x4073cd, "op_0425_voice", game_op_voice, NULL),
	JUMP_SYS(0x4073ee, "op_0426_map", game_op_map, game_native_map),
	CALL(0x40741b, "vm_parse_args", game_parse_args_regs, REG_BIT(EDX), 0),
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
