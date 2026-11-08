#include "game/game.h"

#include <stddef.h>

#include "game/expr.h"
#include "x86/x86.h"

/* Sorted by address. Keep in step with HLE_GAME_OVERRIDE in src/hle/overrides.h. */
const struct game_unit game_units[] = {
	{ 0x4037fc, "script_eval", game_eval_regs, REG_BIT(EBX) | REG_BIT(EDX), 0 },
	{ 0x4039f7, "script_read_constant", game_read_constant_regs, 0, 0 },
	{ 0x406455, "script_set_flag", game_set_flag_regs, 0, 0 },
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
