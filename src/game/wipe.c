/*
 * The dissolve wipe 0x401a50 (stage 6): op 19 sub-op 4, copying surface 2 to
 * surface 0 in 128 passes, two frames apart.
 *
 * Each pass copies, in 25 rows of the 400-line picture (every 16th line,
 * starting at line `ebx`), the pixels whose bit is set in a 16-byte dither
 * mask. The mask byte `ebx` gets one more bit (from a bit that walks 0x80,
 * 0x40, ... 1) each pass and `ebx` advances by 5 (mod 16), plus one every
 * 8 passes. So after 128 passes every line has every bit. Between passes the
 * wipe waits until two frames have passed since the pass started, pumping
 * messages (0x401bf0); an input bit 0x30 (OK / cancel) ends the waiting for
 * the remaining passes.
 *
 * It pumps messages, so it is pieces joined by call exits like the VM loop
 * (docs/reimpl.md, Stage 4): 0x401a50 runs the set-up and passes until a wait
 * is needed (the pump is a call exit, return address 0x401b90), 0x401b90 is
 * the rest after a pump. The frame lives in emulated memory at the native
 * stack addresses, E = ESP at entry (the return address at [E]), because
 * the compare checks them at each exit:
 *
 *   E+4   pixel counter (the first argument's slot; 0 after a row)
 *   E+8   pitch of surface 0 (the second argument's slot)
 *   E-0x10 .. E-1  the 16 mask bytes
 *   E-0x1c frame counter at the start of the pass
 *   E-0x20 pixels of surface 1 (a1 = 2), E-0x24 of surface 0 (a0 = 0)
 *   E-0x14 &surface table entry of a1, E-0x18 of a0
 *   E-0x28 passes left, E-0x2c cancelled, E-0x30 rows left,
 *   E-0x34 pitch of surface 1, E-0x35 (byte) the walking bit
 *   E-0x3c .. E-0x48  the saved EBX, EBP, ESI, EDI
 * Between the pieces ESP = E - 0x48 (with the return address of the pump on
 * top while it runs). The registers are modelled as native (cpu.r), so no
 * register is scratch.
 *
 * Instruction counts are the native ones, per block (see the listing of
 * 0x401a50); backward jumps poll like the native ones.
 */
#include "game/vm.h"
#include "x86/x86.h"
#include "game/state.h"

#define SURFACES 0x44629eu /* u32[]: surface objects */
#define PITCHES 0x4462aeu  /* u32[]: their pitches */
#define PUMP 0x401bf0u
#define WIPE_PUMPED 0x401b90u

/* the frame addresses from E */
#define MASKB(e) ((e)-0x10u)
#define F_FRAME(e) ((e)-0x1cu)
#define F_LOCK1(e) ((e)-0x20u)
#define F_LOCK0(e) ((e)-0x24u)
#define F_PTR1(e) ((e)-0x14u)
#define F_PTR0(e) ((e)-0x18u)
#define F_PASSES(e) ((e)-0x28u)
#define F_CANCEL(e) ((e)-0x2cu)
#define F_ROWS(e) ((e)-0x30u)
#define F_PITCH1(e) ((e)-0x34u)
#define F_BIT(e) ((e)-0x35u)
#define F_COUNT(e) ((e) + 4u)
#define F_PITCH0(e) ((e) + 8u)

enum where { START, PUMPED };

static int popcount8(uint32_t v)
{
	int n = 0;
	for (; v; v >>= 1)
		n += (int)(v & 1);
	return n;
}

static uint32_t wipe(enum where where, uint32_t e)
{
	uint32_t *r = cpu.r;
	if (where == PUMPED)
		goto pumped;

	/* prologue, the two locks */
	r[ESP] = e - 0x38;
	r[EAX] = 0;
	push32(r[EBX]);
	wr32(e - 0x10, 0);
	push32(r[EBP]);
	wr32(e - 0x0c, 0);
	push32(r[ESI]);
	r[ESI] = rd32(e + 4);
	wr32(e - 0x08, 0);
	wr32(e - 0x04, 0);
	push32(r[EDI]);
	r[ECX] = rd32(SURFACES + 4 * r[ESI]);
	r[EAX] = SURFACES + 4 * r[ESI];
	r[EBX] = 0;
	push32(r[ECX]);
	wr32(F_CANCEL(e), r[EBX]);
	wr8(F_BIT(e), 0x80);
	wr32(F_PTR0(e), r[EAX]);
	X86_COUNT(19);
	game_lock_regs();
	r[EDI] = rd32(e + 8);
	wr32(F_LOCK0(e), r[EAX]);
	r[EDX] = rd32(SURFACES + 4 * r[EDI]);
	r[EAX] = SURFACES + 4 * r[EDI];
	push32(r[EDX]);
	wr32(F_PTR1(e), r[EAX]);
	X86_COUNT(7);
	game_lock_regs();
	r[ECX] = rd32(PITCHES + 4 * r[EDI]);
	wr32(F_LOCK1(e), r[EAX]);
	r[EAX] = rd32(PITCHES + 4 * r[ESI]);
	r[ESP] += 8;
	wr32(F_PITCH0(e), r[EAX]);
	wr32(F_PITCH1(e), r[ECX]);
	wr32(F_PASSES(e), 0x80);
	X86_COUNT(7);

pass: /* 0x401ad5 */
	{
		uint32_t bit = rd8(F_BIT(e));
		uint32_t m = rd8(MASKB(e) + r[EBX]) | bit;
		uint32_t frame = RT(frame);
		r[EAX] = r[EBX] * rd32(F_PITCH0(e));
		r[ESI] = r[EBX] * rd32(F_PITCH1(e));
		wr32(F_FRAME(e), frame);
		r[EDX] = rd32(F_LOCK0(e));
		wr8(MASKB(e) + r[EBX], m);
		r[ECX] = rd32(F_LOCK1(e));
		r[EAX] += r[EDX];
		r[ESI] += r[ECX];
		wr32(F_ROWS(e), 0x19);
		X86_COUNT(15);
		/* the 25 rows */
		for (;;) {
			r[EBP] = r[ESI];
			r[EDI] = r[EAX];
			r[EBP] -= r[EAX];
			wr32(F_COUNT(e), 0x280);
			uint32_t dst = r[EDI];
			m = rd8(MASKB(e) + r[EBX]);
			for (uint32_t i = 0; i < 640; i++) {
				if (m & (0x80u >> (i & 7)))
					wr8(dst + i, rd8(dst + i + r[EBP]));
				if (i < 639)
					X86_POLL(0x401b3c);
			}
			X86_COUNT(5 + 640 * 10 + 80 + 2 * 80 * popcount8(m));
			r[EDI] = dst + 640;
			wr32(F_COUNT(e), 0);
			r[ECX] = rd32(F_PITCH0(e)) << 4;
			r[EDX] = rd32(F_PITCH1(e));
			r[EAX] += r[ECX];
			r[ECX] = rd32(F_ROWS(e));
			r[EDX] <<= 4;
			r[ESI] += r[EDX];
			r[ECX]--;
			wr32(F_ROWS(e), r[ECX]);
			X86_COUNT(10);
			if (!r[ECX])
				break;
			X86_POLL(0x401b59);
		}
		/* the next bit; every 8th pass the next mask byte */
		bit = (rd8(F_BIT(e)) << 1) & 0xff;
		wr8(F_BIT(e), bit);
		X86_COUNT(4);
		if (!bit) {
			wr8(F_BIT(e), 1);
			r[EBX]++;
			X86_COUNT(2);
		}
		r[EAX] = rd32(F_CANCEL(e));
		r[EBX] = (r[EBX] + 5) & 0xf;
		X86_COUNT(5);
		if (r[EAX])
			goto tail;
		/* wait for two frames since the pass started */
		r[EAX] = RT(frame);
		r[ESI] = rd32(F_FRAME(e));
		r[EAX] -= r[ESI];
		X86_COUNT(5);
		if ((int32_t)r[EAX] >= 2)
			goto tail;
		push32(WIPE_PUMPED);
		X86_COUNT(1);
		return PUMP;
	}

pumped: /* 0x401b90: after the pump (ESP = E - 0x48) */
	X86_COUNT(2);
	if (RT(input_bits) & 0x30) {
		wr32(F_CANCEL(e), 1);
		X86_COUNT(1);
		goto tail;
	}
	r[ECX] = RT(frame) - r[ESI];
	X86_COUNT(4);
	if ((int32_t)r[ECX] < 2) {
		X86_POLL(0x401ba4);
		push32(WIPE_PUMPED);
		X86_COUNT(1);
		return PUMP;
	}
	X86_COUNT(1); /* jmp 0x401bb0 */

tail: /* 0x401bb0 */
	r[EAX] = rd32(F_PASSES(e));
	r[EAX]--;
	wr32(F_PASSES(e), r[EAX]);
	X86_COUNT(4);
	if (r[EAX]) {
		X86_POLL(0x401bb9);
		goto pass;
	}

	/* the two unlocks and the epilogue */
	r[EDX] = rd32(F_PTR0(e));
	r[EAX] = rd32(r[EDX]);
	push32(r[EAX]);
	X86_COUNT(4);
	game_unlock_regs();
	r[ECX] = rd32(F_PTR1(e));
	r[EDX] = rd32(r[ECX]);
	push32(r[EDX]);
	X86_COUNT(4);
	game_unlock_regs();
	r[ESP] += 8;
	r[EDI] = pop32();
	r[ESI] = pop32();
	r[EBP] = pop32();
	r[EBX] = pop32();
	r[ESP] += 0x38;
	X86_COUNT(7);
	return pop32();
}

/* 0x401a50, entered by the call of op 19 sub-op 4 (return address on top) */
uint32_t game_wipe(void)
{
	return wipe(START, cpu.r[ESP]);
}

/* 0x401b90: the pump returned */
uint32_t game_wipe_pumped(void)
{
	return wipe(PUMPED, cpu.r[ESP] + 0x48);
}
