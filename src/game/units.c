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
	CALL_SYS(0x4012b0, "surface_unlock", game_unlock_regs, 0),
	/* Stage 6: the dissolve wipe, pieces around its message pump (wipe.c) */
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
	PIECE(0x4060d4, "vm_entry", game_vm_entry, AT_LOOP, -1, 0, 1, NULL),
	CALL(0x406455, "script_set_flag", game_set_flag_regs, 0, 0),
	JUMP_SYS(0x406842, "op_0414_menu_items", game_op_menu_items, NULL),
	JUMP_SYS(0x406916, "op_0415_cursor", game_op_cursor, NULL),
	JUMP_SYS(0x406b05, "op_17_clear_text", game_op_clear_text, NULL),
	JUMP_SYS(0x406b76, "op_0417_fill_rect", game_op_fill_rect, NULL),
	JUMP_SYS(0x406bcb, "op_0418_xor_rect", game_op_xor_rect, NULL),
	JUMP_SYS(0x406c20, "op_0419_copy_rect", game_op_copy_rect, NULL),
	JUMP_SYS(0x406c85, "op_041a_swap_rect", game_op_swap_rect, NULL),
	JUMP_SYS(0x406cea, "op_041b_masked_rect", game_op_masked_rect, NULL),
	JUMP_SYS(0x406d8b, "op_041c_load_file", game_op_load_file, NULL),
	CALL(0x406dff, "anim_reset", game_anim_reset, 0, 0),
	JUMP_SYS(0x406e11, "op_041d_picture", game_op_picture, NULL),
	JUMP_SYS(0x406f06, "op_0416_palette", game_op_palette, NULL),
	JUMP_SYS_R(0x407096, "op_1a_anim", game_op_anim, NULL, REG_BIT(EDI)),
	RESUME(0x4070b6, "op_1a_anim_done", game_op_anim_done, AT_LOOP, 1),
	JUMP_SYS(0x407204, "op_19_misc", game_op_misc, NULL),
	/* 3 words: the saved ESI, 2 and 0 pushed before the wipe's call */
	RESUME_SYS(0x40738b, "op_19_wipe_done", game_op_wipe_done, AT_LOOP, 3),
	JUMP_SYS(0x4073a5, "op_0424_music", game_op_music, NULL),
	JUMP_SYS(0x4073cd, "op_0425_voice", game_op_voice, NULL),
	/* the map (src/game/map_pump.c): the handler runs direct C code through ctl_piece */
	{ 0x4073ee, "op_0426_map", NULL, game_op_map, 0 },
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
