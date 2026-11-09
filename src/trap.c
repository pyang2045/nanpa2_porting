#include "trap.h"

#include <stdio.h>
#include <stdlib.h>

#include "game/state.h"
#include "dev/dev.h"
#include "game/gmem.h"
#include "game/vm.h"
#include "platform/clock.h"

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

void game_trace_fetch(uint32_t off)
{
	static int vm = -1;
	if (vm < 0)
		vm = getenv("NANPA2_VMTRACE") != NULL;
	dev_vm_fetch(off);
	if (vm)
		fprintf(stderr, "vm %04x op %02x %02x %02x frame %u us %llu\n", off, rd8(0x435e78 + off),
			rd8(0x435e79 + off), rd8(0x435e7a + off), RT(frame), (unsigned long long)clock_us());
}

void game_trace_hit_table(uint32_t table)
{
	dev_hit_table(table);
}
