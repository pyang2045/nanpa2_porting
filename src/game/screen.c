/*
 * The palette (stage 5B): the two colour tables, the DirectDraw palette
 * entries built from them, the fade between the displayed entries and the
 * tables, and the wait for the fade to end.
 *
 * The game keeps its colours in two tables (PAL_BASE: 16 u16 in 4:4:4, the
 * interface colours; PAL_PICTURE: 240 u16 in 5:5:5, the picture's) and
 * builds 256 PALETTEENTRY records at PAL_ENTRIES from them. A fade
 * (0x401d30) stores, per entry and channel, the distance from the entries
 * displayed now to the tables, and the start colours at PAL_START; the
 * 16 ms timer callback (src/hle/hle.c, native 0x4028c0) then moves the
 * entries 16 steps along it, and clears PAL_FADING at the end.
 * Semantics: re-notes.md "Palette and fades".
 *
 * Instruction charges are the native routines' counts (their own
 * instructions, the `ret` included, not the `call`).
 */
#include "game/vm.h"
#include "x86/x86.h"

#define PAL_BASE 0x44530cu     /* u16[16]: 0x0GRB, one nibble per channel */
#define PAL_PICTURE 0x445eaeu  /* u16[240]: 5:5:5, red in bits 10-14 */
#define PAL_SAVED 0x44608eu    /* the same 16 + 240 colours, saved (0x40756c) */
#define PAL_ENTRIES 0x4ac07cu  /* PALETTEENTRY[256]: red, green, blue, flags */
#define PAL_DELTAS 0x4ab060u   /* i32[256][3]: target - displayed, per channel */
#define PAL_START 0x4aac58u    /* the entries as the fade started */
#define PAL_FADING 0x4ac504u   /* u32: a fade is running */
#define PAL_OBJECT 0x4ac4f4u   /* the DirectDraw palette object */
#define VM_FLAGS9 0x4452e0u    /* u8 @[9]: bit 4 keeps the palette tables as they are */
#define PAL_BASE_N 16u
#define PAL_PICTURE_N 240u

/* 0x40756c / 0x407599: save the tables to PAL_SAVED, or put them back,
 * unless @[9] bit 4 is set. Registers are kept. */
static void palette_copy_tables(int save)
{
	X86_COUNT(2);
	if (rd8(VM_FLAGS9) & 0x10) {
		X86_COUNT(1);
		return;
	}
	for (uint32_t i = 0; i < PAL_BASE_N / 2; i++) {
		if (save)
			wr32(PAL_SAVED + 4 * i, rd32(PAL_BASE + 4 * i));
		else
			wr32(PAL_BASE + 4 * i, rd32(PAL_SAVED + 4 * i));
	}
	for (uint32_t i = 0; i < PAL_PICTURE_N / 2; i++) {
		uint32_t o = PAL_BASE_N * 2 + 4 * i;
		if (save)
			wr32(PAL_SAVED + o, rd32(PAL_PICTURE + 4 * i));
		else
			wr32(PAL_PICTURE + 4 * i, rd32(PAL_SAVED + o));
	}
	X86_COUNT(14 + 8 + 0x78);
}

void game_palette_save(void)    /* 0x40756c */
{
	palette_copy_tables(1);
}

void game_palette_restore(void) /* 0x407599 */
{
	palette_copy_tables(0);
}

/* 0x40754d: all 16 + 240 colours = v. */
void game_palette_fill(uint32_t v)
{
	for (uint32_t i = 0; i < PAL_BASE_N; i++)
		wr16(PAL_BASE + 2 * i, v);
	for (uint32_t i = 0; i < PAL_PICTURE_N; i++)
		wr16(PAL_PICTURE + 2 * i, v);
	X86_COUNT(11 + PAL_BASE_N + PAL_PICTURE_N);
}

/* the stdcall method of a COM object at a vtable offset: the arguments
 * (without the object) are on the stack */
static void com_call(uint32_t obj, uint32_t off, uint32_t nargs)
{
	game_ncall(rd32(rd32(obj) + off), nargs);
}

/* 0x401ca0: build the 256 entries from the tables and give them to the
 * DirectDraw palette (SetEntries(0, 0, 256, entries)). */
void game_palette_upload(void)
{
	for (uint32_t i = 0; i < PAL_BASE_N; i++) {
		uint32_t w = rd16(PAL_BASE + 2 * i), e = PAL_ENTRIES + 4 * i;
		wr8(e, w & 0xf0);
		wr8(e + 1, (w >> 4) & 0xf0);
		wr8(e + 2, (w & 0x0f) << 4);
	}
	for (uint32_t i = 0; i < PAL_PICTURE_N; i++) {
		uint32_t w = rd16(PAL_PICTURE + 2 * i), e = PAL_ENTRIES + 4 * (PAL_BASE_N + i);
		wr8(e, (w >> 7) & 0xf8);
		wr8(e + 1, (w >> 2) & 0xf8);
		wr8(e + 2, (w & 0x1f) << 3);
	}
	uint32_t obj = rd32(PAL_OBJECT);
	push32(PAL_ENTRIES);
	push32(0x100);
	push32(0);
	push32(0);
	push32(obj);
	com_call(obj, 0x18, 5);
	cpu.r[ECX] = rd32(obj); /* the vtable the call went through */
	X86_COUNT(3 + 14 * PAL_BASE_N + 2 + 16 * PAL_PICTURE_N + 8 + 2);
}

/* 0x401d30: start a fade from the displayed entries to the tables. Out:
 * EAX the last blue distance, ECX 0 (the registers a script op 04 16 3
 * leaves for the VM: ECX's upper half is read by ops 04 1f). */
void game_fade_start(void)
{
	for (uint32_t i = 0; i < PAL_BASE_N; i++) {
		uint32_t w = rd16(PAL_BASE + 2 * i), e = PAL_ENTRIES + 4 * i, d = PAL_DELTAS + 12 * i;
		wr32(d, (w & 0xf0) - rd8(e));
		wr32(d + 4, ((w >> 4) & 0xf0) - rd8(e + 1));
		wr32(d + 8, ((w & 0x0f) << 4) - rd8(e + 2));
	}
	for (uint32_t i = 0; i < PAL_PICTURE_N; i++) {
		uint32_t w = rd16(PAL_PICTURE + 2 * i), e = PAL_ENTRIES + 4 * (PAL_BASE_N + i);
		uint32_t d = PAL_DELTAS + 12 * (PAL_BASE_N + i);
		wr32(d, ((w >> 7) & 0xf8) - rd8(e));
		wr32(d + 4, ((w >> 2) & 0xf8) - rd8(e + 1));
		wr32(d + 8, ((w & 0x1f) << 3) - rd8(e + 2));
		cpu.r[EAX] = ((w & 0x1f) << 3) - rd8(e + 2);
	}
	for (uint32_t i = 0; i < 0x100; i++)
		wr32(PAL_START + 4 * i, rd32(PAL_ENTRIES + 4 * i));
	wr32(PAL_FADING, 1);
	cpu.r[ECX] = 0; /* rep movsd */
	X86_COUNT(6 + 26 * PAL_BASE_N + 3 + 27 * PAL_PICTURE_N + 4 + 1 + 0x100 + 4);
}

/* 0x401e10 */
uint32_t game_fade_active(void)
{
	X86_COUNT(2);
	return rd32(PAL_FADING);
}
