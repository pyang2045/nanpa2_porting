/*
 * Traps and probes.
 *
 * The original did not check what its scripts asked of it: an opcode that is not in its tables, a
 * block result at the top level, a division by zero in an expression, a sprite pattern with no
 * cells, a file that runs past the data. Where the C engine has no defined behaviour to
 * reproduce, it stops with a trap that names the native address of the code that would have run
 * (the `pc`; 0 where there is none), the game's frame and the script being run. The dev layer
 * can hook a trap (the exploration bot reports where it stood).
 */
#ifndef NANPA2_TRAP_H
#define NANPA2_TRAP_H

#include <stdint.h>

_Noreturn void game_trap(uint32_t pc, const char *why);
/* an access outside the game state (a G address that is not in G) */
_Noreturn void game_fault(uint32_t addr);
/* the one arithmetic fault the script expressions can reach (division by zero) */
_Noreturn void game_divide_error(void);
/* called by game_trap before it aborts (dev logging), may be NULL */
extern void (*game_trap_hook)(void);

/* NANPA2_PROBE=1: a timing line for the places the native code was probed (fades, wipes, ...) */
void game_probe(uint32_t pc);

#endif
