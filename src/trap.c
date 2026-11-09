#include "trap.h"

#include <stdio.h>
#include <stdlib.h>

#include "game/state.h"
#include "hle/clock.h"
#include "hle/dev.h"
#include "game/vm.h"

void (*game_trap_hook)(void);

_Noreturn void game_trap(uint32_t pc, const char *why)
{
	fprintf(stderr, "game trap at %08x: %s\n  frame %u, script %.32s, script offset %04x\n", pc, why,
		RT(frame), (const char *)G.save.name, vm_pc);
	if (game_trap_hook)
		game_trap_hook();
	abort();
}

_Noreturn void game_fault(uint32_t addr)
{
	fprintf(stderr, "game memory fault at %08x\n", addr);
	game_trap(0, "an address outside the game state");
}

_Noreturn void game_divide_error(void)
{
	game_trap(0, "divide error");
}

void game_probe(uint32_t pc)
{
	static int on = -1;
	if (on < 0)
		on = getenv("NANPA2_PROBE") != NULL;
	if (on)
		fprintf(stderr, "probe %06x %u ms frame %u us %llu\n", pc, dev_now(), RT(frame),
			(unsigned long long)clock_us());
}
