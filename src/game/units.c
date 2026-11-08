#include "game/game.h"

#include <stddef.h>

#include "game/expr.h"
#include "game/vm.h"
#include "x86/x86.h"

/* Registers dead where an opcode handler goes back to the VM loop 0x4060f9
 * (only ESP, EBP and ESI are live there, tools/liveness.py). */
#define AT_LOOP (REG_BIT(EAX) | REG_BIT(ECX) | REG_BIT(EDX) | REG_BIT(EBX) | REG_BIT(EDI))

#define CALL(addr, name, fn, scratch, flags) { addr, name, fn, NULL, scratch, flags }
#define JUMP(addr, name, fn, scratch) { addr, name, NULL, fn, scratch, 0 }

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
	JUMP(0x406638, "op_0411_continue", game_op_continue, 0),
	JUMP(0x40663b, "op_0412_break", game_op_break, 0),
	JUMP(0x40663e, "op_12_set_label", game_op_set_label, AT_LOOP),
	CALL(0x4068e1, "vm_menu_item", game_menu_item_regs, REG_BIT(EDX), 0),
	JUMP(0x4069be, "op_18_text_colour", game_op_text_colour, AT_LOOP),
	CALL(0x406dc4, "vm_string_arg", game_string_arg_regs, 0, 0),
	/* not enabled in overrides.h yet (no call seen under the diff) */
	JUMP(0x4070bc, "op_0420_click_handler", game_op_click_handler, AT_LOOP),
	JUMP(0x4071a4, "op_0423_hit_test", game_op_hit_test, AT_LOOP),
	JUMP(0x40739b, "op_0422_args_only", game_op_args_only, AT_LOOP),
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
