/*
 * Script expressions and flags (re-notes.md "Script expressions").
 */
#ifndef NANPA2_GAME_EXPR_H
#define NANPA2_GAME_EXPR_H

#include <stdint.h>

/* Flag n (0-15). */
uint32_t script_flag(uint32_t n);
/* Set flag n to v, values above 15 stored as 15 (only the low 16 bits of v
 * count, as in the native code). */
void script_set_flag(uint32_t n, uint32_t v);

/* Evaluate the RPN expression at *pc (a script offset) up to its 0x03 end
 * token; *pc moves past it. Returns the value. A token that cannot start an
 * expression ends it at once: *bare_token gets it (else -1) and it is the
 * value returned. Malformed
 * expressions (stack underflow, leftover values, an unknown token after a
 * value) trap: the native code would return into garbage. *insns is the
 * number of instructions the native loop runs for it (CPU-time estimate). */
uint32_t script_eval(uint32_t *pc, int *bare_token, uint32_t *insns);
/* tokens seen by script_eval, by value (the replay's coverage report) */
extern uint64_t script_eval_coverage[256];

/* Read one constant operand at *pc. Returns 1 with *value set, or 0 when
 * the byte at *pc is not a constant token (*value = that byte; *pc still
 * moves past it). */
int script_read_constant(uint32_t *pc, uint32_t *value);

#endif
