/*
 * G addresses (R4, docs/refactor.md "G addresses").
 *
 * The game state G (state.h) is one object laid out as the original's data area, and the
 * original's scripts, tables and saved words refer to it by address: a script segment base, the
 * program counter's segment, an animation's start and program counter, a label, a string
 * argument. Those values are kept as they were, numbered as the original's addresses
 * (0x431000-0x4c0000) and used as *indices into G*: g_ptr(a, n) is `(uint8_t *)&G + (a -
 * DATA_BASE)` after a bounds check, and rd8..wr32 read and write through it, little-endian.
 *
 * Why not C pointers: the unchecked indices of the scripts (a slot, a surface number, an offset
 * in a segment past its end) read or write the bytes after the member they index, as natively;
 * that needs G to be contiguous and an address to be a position in it. Why the original's
 * numbering: the FLAG file holds the segment bases, and the checkpoints hash G, so the values
 * stay what they were; every address the C names has a comment with its native counterpart.
 * No G address is a host pointer, and an address outside G traps (game_fault).
 */
#ifndef NANPA2_GAME_GMEM_H
#define NANPA2_GAME_GMEM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "game/state.h"
#include "trap.h"

/* the bytes [a, a + n) of G */
static inline uint8_t *g_ptr(uint32_t a, uint32_t n)
{
	if (a < DATA_BASE || a > DATA_END || n > DATA_END - a)
		game_fault(a);
	return (uint8_t *)&G + (a - DATA_BASE);
}

static inline uint32_t rd8(uint32_t a) { return *g_ptr(a, 1); }
static inline uint32_t rd16(uint32_t a) { uint16_t v; memcpy(&v, g_ptr(a, 2), 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, g_ptr(a, 4), 4); return v; }
static inline void wr8(uint32_t a, uint32_t v) { *g_ptr(a, 1) = (uint8_t)v; }
static inline void wr16(uint32_t a, uint32_t v) { uint16_t t = (uint16_t)v; memcpy(g_ptr(a, 2), &t, 2); }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(g_ptr(a, 4), &v, 4); }

#endif
