/*
 * Differential test records (docs/reimpl.md "Differential harness").
 *
 * A record is one call of a unit as the lifted original ran it: the CPU on
 * entry (ESP pointing at the return address), every byte it read before
 * writing it (its inputs), and the CPU and every byte it wrote at or above
 * the entry ESP on return (its outputs; writes below are dead stack). For an
 * opcode handler (a jump unit) the entry ESP is the handler's own and the
 * record also holds the pc it left by.
 * Records hold game memory, so they stay in the build folder.
 */
#ifndef NANPA2_GAME_DIFF_H
#define NANPA2_GAME_DIFF_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "game/game.h"
#include "game/trace.h"
#include "x86/x86.h"

struct diff_byte {
	uint32_t addr;
	uint8_t value;
};

struct diff_record {
	uint32_t unit;
	uint32_t in_r[8], in_flags;
	uint32_t out_r[8], out_flags;
	uint32_t out_pc; /* jump units: the exit pc; called units: 0 */
	struct diff_byte *in, *out;
	uint32_t n_in, n_out, cap_in, cap_out;
};

/* The native stack: the 1 MB below STACK_TOP. A write is dead (not an
 * output) only if it is on the stack below the entry ESP; game globals
 * (0x4xxxxx) and the heap lie outside and always count. */
#define DIFF_STACK_FLOOR (STACK_TOP - 0x100000u)
static inline int diff_kept(uint32_t addr, uint32_t entry_esp)
{
	return addr >= entry_esp || addr < DIFF_STACK_FLOOR;
}

uint32_t diff_flags(const struct cpu *c);   /* CF PF ZF SF DF OF as EFLAGS */
void diff_set_cpu(const uint32_t r[8], uint32_t flags);

/* Fill rec from the oracle's run: entry CPU, exit CPU, its trace. */
void diff_record_from_trace(struct diff_record *rec, uint32_t unit, const struct cpu *in,
                            const struct cpu *out, const struct trace_set *t);
/* Check a reimplementation run (cpu now, memory now, its trace t, for a
 * jump unit the pc it returned) started from rec's inputs against rec's
 * outputs. 0 if equal, else writes the first differences to why. */
int diff_check(const struct game_unit *u, const struct diff_record *rec, uint32_t pc,
               const struct trace_set *t, char *why, size_t cap);

/* Version 2 adds out_pc after out_flags; version 1 files are still read. */
#define DIFF_MAGIC "N2DIFF2\n"
#define DIFF_MAGIC_V1 "N2DIFF1\n"
int diff_write(FILE *f, const struct diff_record *rec);
/* 1 ok, 0 end of file, -1 corrupt; version 1 or 2 */
int diff_read(FILE *f, struct diff_record *rec, int version);
uint64_t diff_input_hash(const struct diff_record *rec);
void diff_record_free(struct diff_record *rec);

#endif
