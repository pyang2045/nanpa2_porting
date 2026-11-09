/*
 * Dispatch of the units: the game functions and script opcode handlers that
 * are C (src/game), entered by their native address from the interpreter
 * (hle_call for called units, hle_jump for pieces and handlers). R2
 * (docs/refactor.md) replaces this dispatch by ordinary calls; until the last
 * unit is converted it is all there is. The differential harness that used
 * to sit here (NANPA2_DIFF, the fuzzers, the oracle runs) is gone: replay
 * checks the engine now.
 */
#include "game/game.h"

#include "game/vm.h"
#include "x86/x86.h"

int game_call(uint32_t addr)
{
	const struct game_unit *u = game_unit_find(addr);
	if (!u || !u->run)
		return 0;
	u->run();
	return 1;
}

/* The ends of a piece that the native code finishes with a few instructions
 * of its own: the VM's quit (0x40638d: mov esp, [0x445cdc]; then pop ebp, ret
 * to WinMain), and the restart of 04 1e from a nested VM (0x406fc8, jmp
 * 0x4060f9). */
static uint32_t finish_exit(uint32_t pc)
{
	switch (pc) {
	case 0x40638d:
		cpu.r[ESP] = RT(top_esp);
		X86_COUNT(1);
		/* fall through */
	case 0x406393: /* the quit of a piece that already reset ESP */
	case 0x40472f:
		cpu.r[EBP] = pop32();
		pc = pop32();
		X86_COUNT(2);
		return pc;
	case 0x406fc8:
		X86_COUNT(1);
		X86_POLL(0x406fc8);
		return 0x4060f9;
	}
	return pc;
}

int game_jump(uint32_t addr, uint32_t *next)
{
	const struct game_unit *u = game_unit_find(addr);
	if (!u || !u->jump)
		return 0;
	*next = finish_exit(u->jump());
	if (u->poll_self && *next == addr)
		X86_POLL(addr);
	return 1;
}
