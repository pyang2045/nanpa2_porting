/*
 * The dissolve wipe 0x401a50 (stage 6): op 19 sub-op 4, copying surface 2 to
 * surface 0 in 128 passes, two frames apart.
 *
 * Each pass copies, in 25 rows of the 400-line picture (every 16th line,
 * starting at line `row0`), the pixels whose bit is set in a 16-byte dither
 * mask. The mask byte `row0` gets one more bit (from a bit that walks 0x80,
 * 0x40, ... 1) each pass and `row0` advances by 5 (mod 16), plus one every
 * 8 passes. So after 128 passes every line has every bit. Between passes the
 * wipe waits until two frames have passed since the pass started, pumping
 * messages (0x401bf0); an input bit 0x30 (OK / cancel) ends the waiting for
 * the remaining passes.
 *
 * It is one function (R2): the native code is two pieces around its message
 * pump, which a loop replaces. The native frame (the pixel counter, the
 * mask on the stack, the walking bit, the saved registers) is plain locals.
 * Like the native code it ignores the pump's result: a WM_QUIT the pump
 * takes off the queue is gone and the wipe runs on (the game's own loops
 * see the next one).
 *
 * Instruction counts are the native ones, per block (see the listing of
 * 0x401a50); backward jumps poll like the native ones.
 */
#include "game/ctl.h"
#include "game/vm.h"
#include "x86/x86.h"
#include "game/state.h"

#define SURFACES 0x44629eu /* u32[]: surface objects */
#define PITCHES 0x4462aeu  /* u32[]: their pitches */

static int popcount8(uint32_t v)
{
	int n = 0;
	for (; v; v >>= 1)
		n += (int)(v & 1);
	return n;
}

/* 0x401a50 (cdecl s0, s1: the screen's surface number 0 and 2): copy surface
 * s1 over s0 in the dissolve. Returns EAX as the last unlock leaves it: op
 * 19 stores it as S. */
uint32_t game_wipe(uint32_t s0, uint32_t s1)
{
	uint8_t mask[16] = { 0 };
	uint32_t row0 = 0;      /* the first line of the pass's rows */
	uint32_t bit = 0x80;    /* the bit this pass adds to the mask byte */
	uint32_t cancelled = 0;
	x86_probe(0x401a50);
	ctl_tlog("wipe+", 0);

	/* prologue, the two locks */
	COST(19);
	uint32_t base0 = game_surface_lock(rd32(SURFACES + 4 * s0));
	COST(7);
	uint32_t base1 = game_surface_lock(rd32(SURFACES + 4 * s1));
	uint32_t pitch1 = rd32(PITCHES + 4 * s1), pitch0 = rd32(PITCHES + 4 * s0);
	uint32_t passes = 0x80;
	COST(7);

	for (;;) { /* 0x401ad5: a pass */
		uint32_t frame0 = RT(frame);
		mask[row0] |= (uint8_t)bit;
		uint32_t dst = base0 + row0 * pitch0, src = base1 + row0 * pitch1;
		COST(15);
		for (uint32_t rows = 0x19; rows;) {
			uint8_t m = mask[row0];
			uint8_t *d = x86_ptr(dst, 640);
			const uint8_t *s = x86_ptr(src, 640);
			for (uint32_t i = 0; i < 640; i++) {
				if (m & (0x80u >> (i & 7)))
					d[i] = s[i];
				if (i < 639)
					POLL(0x401b3c);
			}
			COST(5 + 640 * 10 + 80 + 2 * 80 * popcount8(m));
			dst += pitch0 << 4;
			src += pitch1 << 4;
			rows--;
			COST(10);
			if (rows)
				POLL(0x401b59);
		}
		/* the next bit; every 8th pass the next mask byte */
		bit = (bit << 1) & 0xff;
		COST(4);
		if (!bit) {
			bit = 1;
			row0++;
			COST(2);
		}
		row0 = (row0 + 5) & 0xf;
		COST(5);
		if (!cancelled) {
			/* wait for two frames since the pass started */
			COST(5);
			if ((int32_t)(RT(frame) - frame0) < 2) {
				COST(1);
				for (;;) {
					ctl_pump(); /* 0x401bf0; the native code ignores its result */
					COST(2);
					if (RT(input_bits) & 0x30) {
						cancelled = 1;
						COST(1);
						break;
					}
					COST(4);
					if ((int32_t)(RT(frame) - frame0) >= 2) {
						COST(1); /* jmp 0x401bb0 */
						break;
					}
					POLL(0x401ba4);
					COST(1);
				}
			}
		}
		/* 0x401bb0 */
		passes--;
		COST(4);
		if (!passes)
			break;
		POLL(0x401bb9);
	}

	/* the two unlocks and the epilogue */
	COST(4);
	game_surface_unlock(rd32(SURFACES + 4 * s0));
	COST(4);
	uint32_t r = game_surface_unlock(rd32(SURFACES + 4 * s1));
	COST(7);
	ctl_tlog("wipe-", 0);
	return r;
}
