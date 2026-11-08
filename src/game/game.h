/*
 * Game logic reimplemented in C (docs/reimpl.md).
 *
 * Each reimplemented native function is a "unit": an override in
 * tools/recomp.py GAME_OVERRIDES whose lifted original is kept as the
 * oracle (x86_exec(X86_ORACLE_TAG | addr)). Units use the native calling
 * convention of their function: hle_call has popped the return address,
 * arguments are on the stack or in registers, results go to registers and
 * emulated memory.
 */
#ifndef NANPA2_GAME_GAME_H
#define NANPA2_GAME_GAME_H

#include <stdint.h>

#define REG_BIT(r) (1u << (r))

struct game_unit {
	uint32_t addr;
	const char *name;
	/* a called unit (GAME_OVERRIDES) has run; an opcode handler entered
	 * by jump (GAME_JUMP_OVERRIDES) has jump, which returns the pc to go
	 * on at (an exit of the VM, or a return address it popped) */
	void (*run)(void);
	uint32_t (*jump)(void);
	/* registers whose value on return is not part of the contract (bit
	 * REG_BIT(EAX)...); the reason is documented at the function */
	uint32_t scratch_regs;
	/* 1 if a caller reads the arithmetic flags after the call */
	int flags_out;
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

/* The exits of the VM's opcode handlers: the loop head 0x4060f9 and the VM
 * end 0x40638d (zero-terminated, for x86_stop_exits). */
extern const uint32_t game_vm_exits[];

#endif
