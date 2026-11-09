/*
 * The town map, part 2 (stage 5A): the cell grid and the drawing.
 *
 * The view is a W x H grid of 16 x 16 pixel cells (VIEW_W x VIEW_H in M).
 * CELLS (0x4313c8) holds a record per cell, 4 words: the tile of the layer
 * and up to three sprite tiles (+1; 0 = none) laid over it. SHADOW
 * (0x433308) is what was drawn last. A frame is: fill the cells from the
 * tile layer (sub-op 2), overlay the visible sprites' pattern tiles (sub-op
 * 3), then draw the cells that differ from the shadow (sub-op 4); sub-op 6
 * is those three in a row, sub-op 5 draws all and then runs the event check
 * (map_pump.c: it pumps messages).
 *
 * Tiles are 16 x 16 blocks of 4-bit pixels (one byte each) in the tile
 * sheet and the sprite sheet, which are DirectDraw surfaces locked for the
 * duration; TILE_OFFSETS (0x4355fa, a table in the exe) maps a tile number
 * to the block's offset in eighths. Bit 3 of a pixel marks, in a tile, a
 * pixel that stays in front of the sprites and, in a sprite, a transparent
 * one; only the low 3 bits reach the screen. A cell with no sprite tiles
 * is copied with the screen's own bit 3 kept; a cell with sprite tiles is
 * composed in three work planes (WORK) and written out.
 */
#include "game/map.h"
#include "game/state.h"

#define CELLS 0x4313c8u
#define SHADOW 0x433308u
#define SURFACES 0x44629eu    /* u32[]: the surface objects; [0] is the screen */
#define PITCHES 0x4462aeu     /* u32[]: their pitches */
#define SURF_SCREEN 0x44629eu /* the three surfaces the map locks, as the native code names them */
#define SURF_TILES 0x4462a6u
#define SURF_SPRITES 0x4462aau
#define LOCKED_SCREEN 0x435548u  /* their locked pixel pointers */
#define LOCKED_TILES 0x43554cu
#define LOCKED_SPRITES 0x435550u
#define TILE_OFFSETS 0x4355fau   /* u16 per tile number (low 12 bits) */
#define LAYER_TILE 0x435556u     /* u16 x 3: the sprite tiles of the cell being drawn */
#define WORK 0x435248u           /* 3 planes of 256 bytes: tile & sprites, tile bits, tile mask */
#define TILE_PITCH 0x280u        /* a sheet is 640 pixels wide */

#define LOCK 0x401190u   /* cdecl (surface): pixels */
#define UNLOCK 0x4012b0u /* cdecl (surface) */

static uint32_t lock_surface(uint32_t surface_var)
{
	push32(rd32(surface_var));
	X86_COUNT(2);
	game_ncall(LOCK, 0);
	cpu.r[ESP] += 4;
	X86_COUNT(2); /* add esp, mov [LOCKED_*], eax */
	return cpu.r[EAX];
}

static void unlock_surface(uint32_t surface_var)
{
	push32(rd32(surface_var));
	X86_COUNT(2);
	game_ncall(UNLOCK, 0);
	cpu.r[ESP] += 4;
	X86_COUNT(1);
}

/* the three locks and unlocks around a drawing pass (the native code keeps
 * the pointers at LOCKED_*) */
static void lock_all(void)
{
	wr32(LOCKED_SCREEN, lock_surface(SURF_SCREEN));
	wr32(LOCKED_TILES, lock_surface(SURF_TILES));
	wr32(LOCKED_SPRITES, lock_surface(SURF_SPRITES));
}

static void unlock_all(void)
{
	unlock_surface(SURF_SPRITES);
	unlock_surface(SURF_TILES);
	unlock_surface(SURF_SCREEN);
}

/* ---- sub-op 2: the grid from the layer */

/* 0x403b28: for each view row and column that lies inside the map, the
 * cell's tile word is the layer's tile at the view's position and its
 * sprite tiles are cleared. (The row and column limits compare the
 * view-relative index with the map's size, as the native code does.) Out:
 * ECX = 0, EDX's low word the high word of 2 * map width * view y; EAX
 * (S) the last row's leftover. */
void game_map_fill_grid(void)
{
	uint32_t m = map_base();
	uint32_t vx = rd16(m + M_VIEW_X), vy = rd16(m + M_VIEW_Y), mw = rd16(m + M_MAP_W);
	uint32_t prod = ((mw * 2) & 0xffff) * vy;
	uint32_t ax = ((prod & 0xffff) + vx + vx + rd16(m + M_LAYER) + 4) & 0xffff;
	uint32_t ebx = ax, edi = CELLS;
	uint32_t vh = rd16(m + M_VIEW_H), eax = cpu.r[EAX];
	X86_COUNT(13);
	for (uint32_t cx = vh; cx; cx--) {
		uint32_t row = (vh - cx) & 0xffff;
		eax = (eax & 0xffff0000u) | row;
		X86_COUNT(4);
		if (row < rd16(m + M_MAP_H)) {
			uint32_t save_ebx = ebx, save_edi = edi;
			uint32_t vw = rd16(m + M_VIEW_W);
			X86_COUNT(4);
			for (uint32_t c = vw; c; c--) {
				uint32_t col = (vw - c) & 0xffff;
				X86_COUNT(4);
				if (col >= rd16(m + M_MAP_W))
					break;
				wr32(edi, rd16(SEG1 + ebx));
				wr32(edi + 4, 0);
				ebx += 2;
				edi += 8;
				X86_COUNT(6);
				X86_POLL(0x403bb0);
			}
			edi = save_edi + (vw << 3);
			ebx = save_ebx;
			eax = vw << 3;
			X86_COUNT(6);
		}
		ebx = (ebx & 0xffff0000u) | ((ebx + mw + mw) & 0xffff);
		X86_COUNT(3);
		X86_POLL(0x403bcf);
	}
	cpu.r[EAX] = eax;
	cpu.r[ECX] = 0;
	cpu.r[EDX] = (cpu.r[EDX] & 0xffff0000u) | (prod >> 16);
	cpu.r[EBX] = ebx;
	cpu.r[ESI] = SV(map_sel);
	cpu.r[EDI] = edi;
	X86_COUNT(1);
}

/* ---- sub-op 3: the sprites over the grid */

/* 0x403bd2: each sprite that is drawn (bit 1) and not off (bit 7) takes its
 * pattern for its frame: a width, a height and then width * height tile
 * numbers; it also records its footprint (S_WIDTH, S_HEIGHT). The tiles
 * inside the view go (+1) into the first free of the three sprite tile
 * slots of their cell. Out: ESI = the map selector; ECX = 0 if any sprite
 * was drawn; EBP and EAX as the native loops leave them (the last drawn
 * sprite's row index after its rows, W * 8). */
void game_map_overlay_sprites(void)
{
	uint32_t m = map_base();
	uint32_t sel = SV(map_sel);
	uint32_t s = rd16(m + M_SPRITES);
	uint32_t ebp = cpu.r[EBP], eax = cpu.r[EAX], ecx = cpu.r[ECX];
	X86_COUNT(2);
	for (;;) {
		X86_COUNT(2);
		if (rd8(SEG1 + s) == SPRITE_END)
			break;
		uint32_t sp = SEG1 + s;
		X86_COUNT(3); /* push ebx, test, jne */
		if (rd8(sp + S_FLAGS) & SPRITE_OFF) {
			X86_COUNT(3);
		} else if (!(rd8(sp + S_FLAGS) & SPRITE_DRAWN)) {
			X86_COUNT(5);
		} else {
			X86_COUNT(2);
			uint32_t vw = rd16(m + M_VIEW_W);
			uint32_t y = rd16(sp + S_Y), x = rd16(sp + S_X);
			uint32_t ry = (y - rd16(m + M_VIEW_Y)) & 0xffff;
			uint32_t rx = (x - rd16(m + M_VIEW_X)) & 0xffff;
			uint32_t cell = CELLS + ((((vw * ry) & 0xffff) + rx) << 3 & 0xffff);
			uint32_t idx = ((rd8(sp + S_PATTERN) << 3) + rd16(m + M_FRAMES) + rd8(sp + S_FRAME)) & 0xffff;
			uint32_t pat = (rd8(SEG1 + idx) * 2 + rd16(m + M_PATTERNS)) & 0xffff;
			uint32_t p = rd16(SEG1 + pat);
			uint32_t w = rd8(SEG1 + p), h;
			wr8(sp + S_WIDTH, w);
			wr8(0x43555c, w);
			h = rd8(SEG1 + p + 2);
			wr8(sp + S_HEIGHT, h);
			wr8(0x43555d, h);
			uint32_t tiles = p + 4;
			ebp = (ebp & 0xffff0000u) | ry;
			X86_COUNT(37 + 6);
			if (!h || !w)
				x86_trap(0x403bd2, "map: a sprite pattern with no cells");
			for (uint32_t r = 0; r < h; r++) {
				uint32_t cp = cell;
				uint32_t col = rx;
				if (r)
					X86_COUNT(5); /* push ecx, edx, ebp, edi, movzx ecx */
				for (uint32_t k = 0; k < w; k++) {
					X86_COUNT(4);
					if ((col & 0xffff) <= ((vw - 1) & 0xffff)) {
						X86_COUNT(4);
						if ((ebp & 0xffff) <= ((rd16(m + M_VIEW_H) - 1) & 0xffff)) {
							uint32_t t = (rd16(SEG1 + tiles) + 1) & 0xffff;
							X86_COUNT(2);
							if (rd16(cp + 2) == 0) {
								wr16(cp + 2, t);
								X86_COUNT(4);
							} else if (rd16(cp + 4) == 0) {
								wr16(cp + 4, t);
								X86_COUNT(6);
							} else {
								X86_COUNT(6);
								if (rd16(cp + 6) == 0) {
									wr16(cp + 6, t);
									X86_COUNT(1);
								}
							}
						}
					}
					tiles += 2;
					cp += 8;
					col++;
					X86_COUNT(4);
					X86_POLL(0x403d26);
				}
				ebp++;
				cell += vw << 3;
				X86_COUNT(9);
				X86_POLL(0x403d39);
			}
			eax = vw << 3;
			ecx = 0;
			X86_COUNT(3);
		}
		s += SPRITE_SIZE;
		X86_POLL(0x403d3f);
	}
	cpu.r[EAX] = eax;
	cpu.r[ECX] = ecx;
	cpu.r[EBP] = ebp;
	cpu.r[ESI] = sel;
	X86_COUNT(1);
}

/* ---- one cell */

/* the cell with no sprite tiles (0x403f44): the tile, over the screen's
 * bit 3 */
static void cell_tile(uint32_t src, uint32_t dst)
{
	uint32_t pitch = rd32(PITCHES);
	X86_COUNT(2);
	for (uint32_t row = 0; row < 16; row++) {
		for (uint32_t k = 0; k < 4; k++)
			wr32(dst + 4 * k, (rd32(src + 4 * k) & 0x07070707u) | (rd32(dst + 4 * k) & 0x08080808u));
		src += TILE_PITCH;
		dst += pitch;
		X86_COUNT(27);
		X86_POLL(0x403fac);
	}
	X86_COUNT(1);
}

/* 0x404132: the tile into the work planes: pixels as they are; the bits
 * of the pixels whose bit 3 is set; the inverse mask of those pixels */
static void work_load(uint32_t src)
{
	X86_COUNT(2);
	for (uint32_t row = 0; row < 16; row++) {
		for (uint32_t k = 0; k < 4; k++) {
			uint32_t o = row * 16 + 4 * k, t = rd32(src + 4 * k);
			uint32_t b = t & 0x08080808u, m = b - (b >> 3);
			wr32(WORK + o, t);
			wr32(WORK + 0x100 + o, t & m);
			wr32(WORK + 0x200 + o, m ^ 0x07070707u);
		}
		src += TILE_PITCH;
		X86_COUNT(48);
		X86_POLL(0x4041f2);
	}
	X86_COUNT(1);
}

/* 0x4041f9: a sprite tile over plane 1: its opaque pixels (bit 3 clear)
 * replace the pixels, the transparent ones keep them */
static void work_overlay(uint32_t src)
{
	X86_COUNT(2);
	for (uint32_t row = 0; row < 16; row++) {
		for (uint32_t k = 0; k < 4; k++) {
			uint32_t o = row * 16 + 4 * k, p = rd32(src + 4 * k);
			uint32_t b = p & 0x08080808u, m = b - (b >> 3);
			wr32(WORK + o, (rd32(WORK + o) & m) | (p & 0x07070707u));
		}
		src += TILE_PITCH;
		X86_COUNT(40);
		X86_POLL(0x40427e);
	}
	X86_COUNT(1);
}

/* 0x404281: the composed cell to the screen: where the tile's bit 3 was
 * clear the composed pixel, where it was set the tile's own */
static void work_store(uint32_t dst)
{
	uint32_t pitch = rd32(PITCHES);
	X86_COUNT(2);
	for (uint32_t row = 0; row < 16; row++) {
		for (uint32_t k = 0; k < 4; k++) {
			uint32_t o = row * 16 + 4 * k;
			wr32(dst + 4 * k, (rd32(WORK + o) & rd32(WORK + 0x200 + o)) | rd32(WORK + 0x100 + o));
		}
		dst += pitch;
		X86_COUNT(19);
		X86_POLL(0x4042e4);
	}
	X86_COUNT(1);
}

/* the sprite sheet pixels of sprite tile number t (+1; the table gives the
 * offset in eighths) */
static uint32_t sprite_pixels(uint32_t t)
{
	uint32_t i = (t - 1) & 0xfff;
	return (rd16(TILE_OFFSETS + 2 * i) << 3) + rd32(LOCKED_SPRITES);
}

/* 0x403eff: draw the cell at record `cell` onto the screen at offset
 * `dst` of the locked screen (EDI, EDX in the native code): the tile, and
 * the 0-3 sprite tiles over it, the last drawn first. All registers kept. */
static void cell_draw(uint32_t cell, uint32_t dst_off)
{
	uint32_t src = rd32(LOCKED_TILES);
	uint32_t dst = dst_off + rd32(LOCKED_SCREEN);
	uint32_t tile = rd16(cell) & 0xfff;
	src += rd16(TILE_OFFSETS + 2 * tile) << 3;
	uint32_t layers = 0;
	X86_COUNT(9 + 2 + 1); /* up to xor eax, cmp [edi+2], call */
	if (rd16(cell + 2) != 0) {
		layers = 1;
		X86_COUNT(3);
		if (rd16(cell + 4) != 0) {
			layers = 2;
			X86_COUNT(3);
			if (rd16(cell + 6) != 0) {
				layers = 3;
				X86_COUNT(3);
			}
		}
	}
	switch (layers) {
	case 0:
		cell_tile(src, dst);
		break;
	case 1:
		wr16(LAYER_TILE, rd16(cell + 2));
		X86_COUNT(4 + 6 + 2 + 1);
		work_load(src);
		work_overlay(sprite_pixels(rd16(LAYER_TILE)));
		work_store(dst);
		break;
	case 2:
		wr16(LAYER_TILE, rd16(cell + 2));
		wr16(LAYER_TILE + 2, rd16(cell + 4));
		X86_COUNT(7 + 12 + 3 + 1);
		work_load(src);
		work_overlay(sprite_pixels(rd16(LAYER_TILE + 2)));
		work_overlay(sprite_pixels(rd16(LAYER_TILE)));
		work_store(dst);
		break;
	default:
		wr16(LAYER_TILE, rd16(cell + 2));
		wr16(LAYER_TILE + 2, rd16(cell + 4));
		wr16(LAYER_TILE + 4, rd16(cell + 6));
		X86_COUNT(10 + 18 + 4 + 1);
		work_load(src);
		work_overlay(sprite_pixels(rd16(LAYER_TILE + 4)));
		work_overlay(sprite_pixels(rd16(LAYER_TILE + 2)));
		work_overlay(sprite_pixels(rd16(LAYER_TILE)));
		work_store(dst);
		break;
	}
	X86_COUNT(2); /* popad, ret */
}

/* 0x403eff as a unit: EDI = the cell record, EDX = the screen offset; the
 * native code is pushad ... popad. */
void game_map_cell_draw(void)
{
	cell_draw(cpu.r[EDI], cpu.r[EDX]);
}

/* 0x4040f0: clear bit 3 of the 16 x 16 pixels at screen offset EDX of the
 * locked screen. Out: EAX = the last dword written; the rest is kept. */
void game_map_cell_clear_bit3(void)
{
	uint32_t dst = cpu.r[EDX] + rd32(LOCKED_SCREEN);
	uint32_t pitch = rd32(PITCHES), v = 0;
	X86_COUNT(4);
	for (uint32_t row = 0; row < 16; row++) {
		for (uint32_t k = 0; k < 4; k++) {
			v = rd32(dst + 4 * k) & 0x07070707u;
			wr32(dst + 4 * k, v);
		}
		dst += pitch;
		X86_COUNT(14);
		X86_POLL(0x40412d);
	}
	cpu.r[EAX] = v;
	X86_COUNT(3);
}

/* ---- sub-op 4: the cells that changed */

/* the registers the native cell loops hold when they get to the unlocks:
 * EBX / EDI after the last cell of both tables, EDX the screen offset
 * after the last row, ECX = 0 */
static void loop_end_regs(uint32_t cells, uint32_t dst_end)
{
	cpu.r[EBX] = CELLS + cells * 8;
	cpu.r[EDI] = SHADOW + cells * 8;
	cpu.r[EDX] = dst_end;
	cpu.r[ECX] = 0;
	cpu.r[ESI] = SV(map_sel);
}

/* 0x403d45: lock the three surfaces and draw the cells whose record
 * differs from the shadow, copying it there first; then unlock. S (AX) is
 * what the last unlock returns. */
void game_map_redraw_changed(void)
{
	lock_all();
	uint32_t m = map_base();
	uint32_t pitch = rd32(PITCHES);
	uint32_t ebp_save = cpu.r[EBP];
	uint32_t y = rd16(m + M_SCREEN_Y), x = rd16(m + M_SCREEN_X);
	uint32_t row_step = pitch << 4;
	uint32_t dst_row = (x << 4) + y * pitch;
	uint32_t vh = rd16(m + M_VIEW_H), vw = rd16(m + M_VIEW_W);
	uint32_t cell = CELLS, shadow = SHADOW;
	X86_COUNT(12); /* after the locks: push ebp ... movzx ecx */
	for (uint32_t r = 0; r < vh; r++) {
		uint32_t dst = dst_row;
		X86_COUNT(3);
		for (uint32_t c = 0; c < vw; c++) {
			if (rd16(cell) != rd16(shadow)) {
				X86_COUNT(3);
			} else if (rd16(cell + 2) != rd16(shadow + 2)) {
				X86_COUNT(6);
			} else if (rd16(cell + 4) != rd16(shadow + 4)) {
				X86_COUNT(9);
			} else if (rd16(cell + 6) != rd16(shadow + 6)) {
				X86_COUNT(12);
			} else {
				goto same;
			}
			if (1) {
				wr32(shadow, rd32(cell));
				wr32(shadow + 4, rd32(cell + 4));
				X86_COUNT(5);
				cell_draw(shadow, dst);
			}
			goto next;
same:
			X86_COUNT(12);
next:
			cell += 8;
			shadow += 8;
			dst += 0x10;
			X86_COUNT(4);
			X86_POLL(0x403dfc);
		}
		dst_row = dst_row + row_step;
		X86_COUNT(4);
		X86_POLL(0x403e02);
	}
	cpu.r[EBP] = ebp_save;
	loop_end_regs(vw * vh, dst_row);
	X86_COUNT(1);
	unlock_all();
	X86_COUNT(1);
}

/* ---- sub-op 5: every cell */

/* 0x403e30 up to its call of the event check 0x405216: lock the three
 * surfaces, copy each cell record over its shadow, clear bit 3 of its 16 x 16
 * screen pixels (0x4040f0) and draw it (0x403eff). EBP holds the row step
 * (pushed first, and until game_map_redraw_all_end) and the registers are
 * the ones the native loops leave. */
void game_map_redraw_all_begin(void)
{
	lock_all();
	uint32_t m = map_base();
	uint32_t pitch = rd32(PITCHES);
	uint32_t y = rd16(m + M_SCREEN_Y), x = rd16(m + M_SCREEN_X);
	uint32_t row_step = pitch << 4;
	uint32_t dst_row = (x << 4) + y * pitch;
	uint32_t vh = rd16(m + M_VIEW_H), vw = rd16(m + M_VIEW_W);
	uint32_t cell = CELLS, shadow = SHADOW;
	push32(cpu.r[EBP]);
	cpu.r[EBP] = row_step;
	X86_COUNT(12); /* push ebp ... movzx ecx */
	for (uint32_t r = 0; r < vh; r++) {
		uint32_t dst = dst_row;
		X86_COUNT(3);
		for (uint32_t c = 0; c < vw; c++) {
			wr32(shadow, rd32(cell));
			wr32(shadow + 4, rd32(cell + 4));
			cpu.r[EDX] = dst;
			X86_COUNT(4 + 1);
			game_map_cell_clear_bit3();
			cpu.r[EDI] = shadow;
			X86_COUNT(1);
			game_map_cell_draw();
			cell += 8;
			shadow += 8;
			dst += 0x10;
			X86_COUNT(4);
			X86_POLL(0x403ec6);
		}
		dst_row = dst_row + row_step;
		X86_COUNT(4);
		X86_POLL(0x403ecc);
	}
	loop_end_regs(vw * vh, dst_row);
}

/* 0x403ed3: after the event check, restore EBP and unlock; the ret is the
 * caller's */
void game_map_redraw_all_end(void)
{
	cpu.r[EBP] = pop32();
	X86_COUNT(1);
	unlock_all();
	X86_COUNT(1);
}
