/*
 * Script VM state, read and written at its native addresses.
 *
 * The reimplemented game logic (src/game) keeps emulated memory as its only
 * state store, so lifted and reimplemented code see the same variables. This
 * header names those variables; see re-notes.md "Script VM state".
 */
#ifndef NANPA2_GAME_VM_H
#define NANPA2_GAME_VM_H

#include <stdint.h>

#include "x86/x86.h"

/* MES script image (loaded by 0x401050 from the file named at
 * VM_SCRIPT_NAME). The VM's program counter (ESI in the native code) is an
 * offset from here, and segment 0 starts here. */
#define VM_SCRIPT 0x435e78u
#define VM_SCRIPT_NAME 0x444e78u

/* 4-bit script flags, two per byte: flag n is in byte n/2, the low nibble
 * for even n, the high nibble for odd n. */
#define VM_FLAGS 0x444e98u

/* Registers '@' (0x40) to 'Z' (0x5a): 27 u16. Register '@' holds the
 * segment-0 offset of the word array VM_ARRAY (set by 0x406395), so
 * expressions index the array through it. */
#define VM_REGS 0x445298u
#define VM_REG_COUNT 27
#define VM_ARRAY 0x4452ceu

/* Word 7 of that array (@[7]) selects the data segment that array
 * expressions and assignments use. */
#define VM_SEGMENT_SELECT 0x4452dcu

/* Segment base addresses (u32 each): 0 = the script image, 1 = 0x4462be,
 * 2 and 3 = 0x4562be (0x406395). */
#define VM_SEGMENTS 0x44533cu

/* Seed of the script random number generator (u16). */
#define VM_RANDOM_SEED 0x4313c4u

static inline uint32_t vm_script_byte(uint32_t pc)
{
	return rd8(VM_SCRIPT + pc);
}

static inline uint32_t vm_reg(uint32_t r)
{
	return rd16(VM_REGS + 2u * r);
}

static inline void vm_set_reg(uint32_t r, uint32_t v)
{
	wr16(VM_REGS + 2u * r, v);
}

static inline uint32_t vm_segment_base(uint32_t seg)
{
	return rd32(VM_SEGMENTS + 4u * seg);
}

static inline uint32_t vm_segment_select(void)
{
	return rd16(VM_SEGMENT_SELECT);
}

#endif
