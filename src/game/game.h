/*
 * Game logic reimplemented in C (docs/reimpl.md).
 *
 * Each reimplemented native function is a "unit": an override in
 * src/hle/overrides.h (HLE_GAME_OVERRIDE) whose original is kept as the
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
	/* jump units: if set and it returns 1 for the CPU on entry, the
	 * original handler runs instead (hle_jump continues at the tagged
	 * entry), with no comparison: the cases the C handler leaves to the
	 * native code (sub-ops that wait on the timer or pump messages) */
	int (*native)(void);
	/* the C side runs native game code (x86_exec): its diff records
	 * cannot be replayed offline, so none are written */
	int calls_native;
	/* jump units that are the rest of a handler after a call exit: the
	 * words the handler pushed before that call, still on the stack (the
	 * handler's own level, where its oracle ends, is that far up) */
	int stack_words;
	/* bits of EAX that stay compared although EAX is in scratch_regs (a
	 * result in AL: the movers 0x404731...), 0 = none */
	uint32_t eax_live;
	/* a piece of a wait loop that returns its own address while it waits:
	 * game_jump polls the timer (X86_POLL) for each such return, after the
	 * comparison, because the compare holds the timer off */
	int poll_self;
	/* stage 6, the pump chain: the registers that are scratch only where
	 * the unit leaves for exit_pc (the handler tail 0x40740f needs only AX,
	 * but the same piece's exits at a pump need all of them) */
	uint32_t exit_pc, exit_scratch;
	/* stack_words by calculation: how many words from ESP up to and
	 * including the return address of the chain's handler (0x40740f),
	 * where the oracle of a piece inside the 04 26 pump chain ends */
	int (*words_fn)(void);
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
