/*
 * The time model of the game (R3): what the original's busy work cost on the 486-class PC the Taiwan
 * release ran on, in microseconds of game time. docs/refactor.md "R3" has the derivation.
 *
 * Before R3 every unit charged the instruction count of the native code it replaced, at 25 MIPS (a
 * 486DX2-66 at about 2.6 clocks per instruction). Profiling that on the four golden runs (a new game
 * through the Prologue, the town, a load, the menus: 17 million script opcodes, 100 million pumps)
 * showed where the time went: 89 % in the message pump's spin on the 16 ms timer (a wait, not work),
 * 4 % in expression evaluation, 1.1 % in GDI text, 1 % in block skipping, 0.5 % in picture decoding,
 * and under 0.1 % in everything else (the map, animation steps, cursor, blits). The model charges
 * the first five, the dissolve and the fade set-up, and the pump; it differs from the old charge
 * count by 1 % in total over those runs. The constants are the instruction costs / 25.
 *
 * Changing one changes the timing of every recording: bump PACE_VERSION, which a recording carries.
 */
#ifndef NANPA2_GAME_PACE_H
#define NANPA2_GAME_PACE_H

#define PACE_VERSION 1

/* one message pump and the wait loop around it: 500 instructions for PeekMessage on Windows 95 plus
 * about 17 for the loop's own tests and the animation step (3.5 % of the pump's total) */
#define PACE_PUMP_US 21u
/* one round of a wait that does not pump, on the frame counter or the fade flag (a few instructions
 * in the original; charged coarsely so that the loop is not a million host iterations a second) */
#define PACE_SPIN_US 1u
/* an opcode's fetch, dispatch and handler overhead (about 50 instructions) */
#define PACE_OP_US 2u
/* one script expression (the evaluator averages 229 instructions over 17.7 million calls) */
#define PACE_EVAL_US 9u
/* skipping a block: about 5 instructions a byte */
#define PACE_SKIP_US(bytes) (1u + (bytes) / 5u)
/* GDI TextOut of one byte of the string (a cached TrueType glyph, 0.2 ms) */
#define PACE_GLYPH_US 200u
/* copying a character's N x N cell from the scratch surface: 8 instructions a pixel */
#define PACE_CELL_US(n) ((n) * (n) / 3u)
/* decoding a GP4 or ANI file: 70 instructions a compressed byte and 1.2 an output byte */
#define PACE_DECODE_US(in, out) ((uint32_t)(((uint64_t)(in) * 70u + (uint64_t)(out) * 6u / 5u) / 25u))
/* a DirectDraw Blt: 4 pixels per instruction-time plain (rep movsd), 1 with a colour key, + 200 */
#define PACE_BLIT_US(w, h, keyed) ((uint32_t)(((uint64_t)(w) * (uint64_t)(h) / ((keyed) ? 1u : 4u) + 200u) / 25u))
/* one row of the dissolve's pass (every 16th line of the 400: 640 pixels at 10 instructions, plus
 * 160 for each bit set in the row's mask) and the pass is 25 such rows: 7.2 ms on average, 128
 * passes, 0.9 s with the waits skipped */
#define PACE_WIPE_ROW_US(bits) (260u + 64u * (bits) / 10u)
/* starting a palette fade: the delta tables of both palettes */
#define PACE_FADE_US 290u

#endif
