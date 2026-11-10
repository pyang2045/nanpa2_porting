#include "game/state.h"

#include <string.h>


struct game_state G;

/* The start values of the save image (G.save; the rest of G is state_defaults'). A new game and
 * the blank FLAG file are this plus the VM set-up (vm_init). */
void save_defaults(void)
{
	/* 0x444e78: the script the VM starts with (the name buffer, as a FLAG file holds it, ends in the
	 * DOS string terminator bytes the native string had) */
	memcpy(G.save.name, "start.mes\0\r\n$", 13);
	/* 0x4452e1..: the text window and menu inputs the VM starts with */
	G.save.n.input_enable = 0x7e;
	G.save.n.text_right = 79;
	G.save.n.text_bottom = 399;
	G.save.n.text_colour = 7;
	G.save.n.text_line = 16;
	G.save.n.text_advance = 2;
	G.save.n.w_308 = 0xffffffffu;
	/* 0x44530c: the 16 base colours, 0x0GRB with a full nibble per set bit (bit 0 blue, 1 green,
	 * 2 red; the first eight again) */
	for (unsigned i = 0; i < 16; i++)
		G.save.n.palette[i] = (uint16_t)((i & 1 ? 0x00f : 0) | (i & 2 ? 0x0f0 : 0) | (i & 4 ? 0xf00 : 0));
}

/* The start values of G (the exe's initialised .data, as the native start-up found it), defined
 * here as typed fields; everything not set is zero. The native address of each is in state.h. The
 * rest of the exe's .data (function pointers of the native code, its own format and message
 * strings, the MP3 library's tables, which lie in the area the ANI file is later read into) is
 * read by nothing in this engine and is not reproduced. */
static void state_defaults(void)
{
	/* 0x431030: the CD prefix; the start-up search (startup.c) writes its first byte */
	memcpy(G.head.cd_prefix, "A:\\", 4);

	/* 0x4312cc: the animation slots all start at 0xf4 (never run before a script sets them), a reset
	 * slot's script start, the script random number generator's seed */
	for (unsigned i = 0; i < AN_SLOTS; i++)
		G.an.slot[i].start = 0xf4;
	G.an.idle = 0xff;
	G.an.random_seed = 0x8801;

	/* 0x43556a, 0x435e60: the map's event state (no sprite, click or area yet) and the sprite
	 * random generator's seed */
	G.map.v.last_sprite = 0xffff;
	G.map.v.last_click = 0xffff;
	G.map.v.last_area = 0xffff;
	G.map.v.random_seed = 0x8a;
	/* 0x4355fa: tile number -> byte offset in the tile sheet (40 tiles of 2 bytes per row, rows
	 * 1280 bytes apart) */
	for (unsigned i = 0; i < sizeof G.map.v.tile_offsets / sizeof G.map.v.tile_offsets[0]; i++)
		G.map.v.tile_offsets[i] = (uint16_t)(i / 40 * 1280 + i % 40 * 2);

	save_defaults();

	/* 0x445d86: the FLAG file name, its digit written at a save */
	memcpy(G.rt.flag_name, "flag0\0\r\n$", 9);
	/* 0x44628e: the palette fade length */
	G.rt.fade_steps = 8;
	/* 0x445c71: the 7 bytes of the exe's data there are read by no script (checked over the golden
	 * replays and the exploration bot), so they start as zero like the rest of the block */
}

void state_init(void)
{
	memset(&G, 0, sizeof G);
	state_defaults();
}
