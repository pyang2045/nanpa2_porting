/*
 * Game logic reimplemented in C (docs/reimpl.md).
 *
 * Each reimplemented native function is a "unit": an override in
 * src/hle/overrides.h (HLE_GAME_OVERRIDE) entered by its native address.
 * Units use the native calling convention of their function: hle_call has
 * popped the return address, arguments are on the stack or in registers,
 * results go to registers and emulated memory. (R2, docs/refactor.md,
 * replaces this by ordinary calls.)
 */
#ifndef NANPA2_GAME_GAME_H
#define NANPA2_GAME_GAME_H

#include <stdint.h>

#define REG_BIT(r) (1u << (r))

struct game_unit {
	uint32_t addr;
	const char *name;
	/* a called unit has run, which returns to the caller's return address;
	 * a piece or opcode handler has jump, which returns the pc to go on at */
	void (*run)(void);
	uint32_t (*jump)(void);
	/* a piece of a wait loop that returns its own address while it waits:
	 * the timer is polled for each such return (the native backward jump) */
	int poll_self;
};

extern const struct game_unit game_units[];
extern const int game_unit_count;
const struct game_unit *game_unit_find(uint32_t addr);

/* From hle_call (after the return address was popped): run the unit at
 * addr. 0 if addr is not a unit. With NANPA2_DIFF set every call runs the
 * lifted original and the reimplementation from the same state and compares
 * them (src/game/game.c). */
int game_call(uint32_t addr);
/* From hle_jump: run the opcode handler at addr, *next = the pc to go on
 * at. 0 if addr is not a jump unit. */
int game_jump(uint32_t addr, uint32_t *next);

#endif
