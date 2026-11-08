/*
 * Memory access tracing for the differential tests: while a trace runs,
 * every byte touched through rd*()/wr*() (src/x86/x86.h) is noted with its
 * value before the first access.
 */
#ifndef NANPA2_GAME_TRACE_H
#define NANPA2_GAME_TRACE_H

#include <stddef.h>
#include <stdint.h>

enum {
	TRACE_TOUCHED = 1,    /* pre is valid */
	TRACE_READ_FIRST = 2, /* read before any write: an input */
	TRACE_WRITTEN = 4,
};

struct trace_byte {
	uint32_t addr;
	uint8_t state; /* TRACE_* */
	uint8_t pre;   /* value before the first access */
	uint8_t post;  /* value when the trace was collected */
};

struct trace_set {
	struct trace_byte *b; /* sorted by address */
	size_t n, cap;
};

void trace_begin(void); /* forget the previous trace, start recording */
void trace_end(void);   /* stop recording (the trace stays readable) */
/* Append the bytes of the last trace (in address order) to *out. */
void trace_collect(struct trace_set *out);
/* The entry for addr in a collected set, or NULL. */
const struct trace_byte *trace_find(const struct trace_set *s, uint32_t addr);
void trace_set_clear(struct trace_set *s);
void trace_set_free(struct trace_set *s);

#endif
