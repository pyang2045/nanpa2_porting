/*
 * The town map, part 2: the cell grid and the drawing.
 *
 * The view is a W x H grid of 16 x 16 pixel cells (view_w x view_h in M).
 * G.map.cells holds a record per cell: the tile of the layer and up to three
 * sprite tiles (+1; 0 = none) laid over it. G.map.shadow is what was drawn
 * last. A frame is: fill the cells from the tile layer (sub-op 2), overlay the
 * visible sprites' pattern tiles (sub-op 3), then draw the cells that differ
 * from the shadow (sub-op 4); sub-op 6 is those three in a row, sub-op 5
 * draws all and then runs the event check (map_pump.c: it pumps messages).
 *
 * Tiles are 16 x 16 blocks of 4-bit pixels (one byte each) in the tile
 * sheet and the sprite sheet, which are DirectDraw surfaces locked for the
 * duration; the tile offset table (read-only data of the exe) maps a tile
 * number to the block's offset in eighths. Bit 3 of a pixel marks, in a tile,
 * a pixel that stays in front of the sprites and, in a sprite, a transparent
 * one; only the low 3 bits reach the screen. A cell with no sprite tiles is
 * copied with the screen's own bit 3 kept; a cell with sprite tiles is
 * composed in three work planes (G.map.work) and written out.
 *
 * The locked surface pixels are guest addresses of the HLE layer's surfaces;
 * while locked the drawing holds C pointers to them (x86_ptr). The guest
 * addresses stay in G.map.v (the data area is checked).
 */
#include "game/map.h"

#define TILE_PITCH 0x280u /* a sheet is 640 pixels wide */

#define LOCK 0x401190u   /* cdecl (surface): pixels */
#define UNLOCK 0x4012b0u /* cdecl (surface) */

/* the locked pixels of the screen, the tile sheet and the sprite sheet, and
 * the screen's pitch */
static struct {
	uint8_t *screen, *tiles, *sprites;
	uint32_t pitch;
} lk;

static inline uint32_t ld32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void st32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static uint32_t lock_surface(uint32_t surface)
{
	COST(2);
	uint32_t p = HLE(LOCK, surface);
	COST(2); /* add esp, mov [LOCKED_*], eax */
	return p;
}

static uint32_t unlock_surface(uint32_t surface)
{
	COST(2);
	uint32_t eax = HLE(UNLOCK, surface);
	COST(1);
	return eax;
}

/* the three locks and unlocks around a drawing pass */
static void lock_all(void)
{
	MV_SET(locked_screen, lock_surface(RT(surface[0])));
	MV_SET(locked_tiles, lock_surface(RT(surface[2])));
	MV_SET(locked_sprites, lock_surface(RT(surface[3])));
	lk.screen = x86_ptr(MV(locked_screen), 1);
	lk.tiles = x86_ptr(MV(locked_tiles), 1);
	lk.sprites = x86_ptr(MV(locked_sprites), 1);
	lk.pitch = RT(pitch[0]);
}

/* the AX the last unlock leaves */
static uint32_t unlock_all(void)
{
	unlock_surface(RT(surface[3]));
	unlock_surface(RT(surface[2]));
	return unlock_surface(RT(surface[0]));
}

/* ---- sub-op 2: the grid from the layer */

/* 0x403b28: for each view row and column that lies inside the map, the
 * cell's tile word is the layer's tile at the view's position and its
 * sprite tiles are cleared. (The row and column limits compare the
 * view-relative index with the map's size, as the native code does.) Returns
 * the AX it leaves: the last row, or the view width * 8 of the last row that
 * was filled; `eax` if the view has no rows. */
uint32_t game_map_fill_grid(uint32_t eax)
{
	const struct map_rec *m = map_rec();
	uint32_t vx = m->view_x, vy = m->view_y, mw = m->map_w;
	uint32_t prod = ((mw * 2) & 0xffff) * vy;
	uint32_t ebx = ((prod & 0xffff) + vx + vx + m->layer + 4) & 0xffff; /* layer offset */
	struct cell *c = cells();
	uint32_t vh = m->view_h;
	COST(13);
	for (uint32_t cx = vh; cx; cx--) {
		uint32_t row = (vh - cx) & 0xffff;
		eax = (eax & 0xffff0000u) | row;
		COST(4);
		if (row < m->map_h) {
			uint32_t save_ebx = ebx;
			struct cell *save_c = c;
			uint32_t vw = m->view_w;
			COST(4);
			for (uint32_t n = vw; n; n--) {
				uint32_t col = (vw - n) & 0xffff;
				COST(4);
				if (col >= m->map_w)
					break;
				c->tile = seg1_u16(ebx);
				c->spr[0] = c->spr[1] = c->spr[2] = 0;
				ebx += 2;
				c++;
				COST(6);
				POLL(0x403bb0);
			}
			c = save_c + vw;
			ebx = save_ebx;
			eax = vw << 3;
			COST(6);
		}
		ebx = (ebx & 0xffff0000u) | ((ebx + mw + mw) & 0xffff);
		COST(3);
		POLL(0x403bcf);
	}
	COST(1);
	return eax;
}

/* ---- sub-op 3: the sprites over the grid */

/* 0x403bd2: each sprite that is drawn (bit 1) and not off (bit 7) takes its
 * pattern for its frame: a width, a height and then width * height tile
 * numbers; it also records its footprint (width, height). The tiles inside
 * the view go (+1) into the first free of the three sprite tile slots of
 * their cell. Returns the AX it leaves: the view width * 8 if any sprite was
 * drawn, else `eax`. */
uint32_t game_map_overlay_sprites(uint32_t eax)
{
	const struct map_rec *m = map_rec();
	uint32_t s = m->sprites;
	COST(2);
	for (;;) {
		COST(2);
		if (seg1_u8(s) == SPRITE_END)
			break;
		struct sprite *sp = sprite_at(s);
		COST(3); /* push ebx, test, jne */
		if (sp->flags & SPRITE_OFF) {
			COST(3);
		} else if (!(sp->flags & SPRITE_DRAWN)) {
			COST(5);
		} else {
			COST(2);
			uint32_t vw = m->view_w;
			uint32_t ry = (sp->y - m->view_y) & 0xffff;
			uint32_t rx = (sp->x - m->view_x) & 0xffff;
			uint32_t off = (((((vw * ry) & 0xffff) + rx) << 3) & 0xffff);
			struct cell *cell = (struct cell *)((uint8_t *)cells() + off);
			uint32_t idx = ((sp->pattern << 3) + m->frames + sp->frame) & 0xffff;
			uint32_t pat = (seg1_u8(idx) * 2 + m->patterns) & 0xffff;
			uint32_t p = seg1_u16(pat);
			uint32_t w = seg1_u8(p), h;
			sp->width = w;
			MV_SET(draw_w, w);
			h = seg1_u8(p + 2);
			sp->height = h;
			MV_SET(draw_h, h);
			uint32_t tiles = p + 4;
			uint32_t row = ry; /* the low word of EBP */
			COST(37 + 6);
			if (!h || !w)
				x86_trap(0x403bd2, "map: a sprite pattern with no cells");
			for (uint32_t r = 0; r < h; r++) {
				struct cell *cp = cell;
				uint32_t col = rx;
				if (r)
					COST(5); /* push ecx, edx, ebp, edi, movzx ecx */
				for (uint32_t k = 0; k < w; k++) {
					COST(4);
					if ((col & 0xffff) <= ((vw - 1) & 0xffff)) {
						COST(4);
						if ((row & 0xffff) <= ((m->view_h - 1u) & 0xffff)) {
							uint32_t t = (seg1_u16(tiles) + 1) & 0xffff;
							COST(2);
							if (cp->spr[0] == 0) {
								cp->spr[0] = t;
								COST(4);
							} else if (cp->spr[1] == 0) {
								cp->spr[1] = t;
								COST(6);
							} else {
								COST(6);
								if (cp->spr[2] == 0) {
									cp->spr[2] = t;
									COST(1);
								}
							}
						}
					}
					tiles += 2;
					cp++;
					col++;
					COST(4);
					POLL(0x403d26);
				}
				row++;
				cell += vw;
				COST(9);
				POLL(0x403d39);
			}
			eax = vw << 3;
			COST(3);
		}
		s += sizeof(struct sprite);
		POLL(0x403d3f);
	}
	COST(1);
	return eax;
}

/* ---- one cell */

/* the cell with no sprite tiles (0x403f44): the tile, over the screen's
 * bit 3 */
static void cell_tile(const uint8_t *src, uint8_t *dst)
{
	COST(2);
	for (uint32_t row = 0; row < 16; row++) {
		for (uint32_t k = 0; k < 4; k++)
			st32(dst + 4 * k, (ld32(src + 4 * k) & 0x07070707u) | (ld32(dst + 4 * k) & 0x08080808u));
		src += TILE_PITCH;
		dst += lk.pitch;
		COST(27);
		POLL(0x403fac);
	}
	COST(1);
}

/* 0x404132: the tile into the work planes: pixels as they are; the bits
 * of the pixels whose bit 3 is set; the inverse mask of those pixels */
static void work_load(const uint8_t *src)
{
	uint8_t *w = G.map.work;
	COST(2);
	for (uint32_t row = 0; row < 16; row++) {
		for (uint32_t k = 0; k < 4; k++) {
			uint32_t o = row * 16 + 4 * k, t = ld32(src + 4 * k);
			uint32_t b = t & 0x08080808u, m = b - (b >> 3);
			st32(w + o, t);
			st32(w + 0x100 + o, t & m);
			st32(w + 0x200 + o, m ^ 0x07070707u);
		}
		src += TILE_PITCH;
		COST(48);
		POLL(0x4041f2);
	}
	COST(1);
}

/* 0x4041f9: a sprite tile over plane 0: its opaque pixels (bit 3 clear)
 * replace the pixels, the transparent ones keep them */
static void work_overlay(const uint8_t *src)
{
	uint8_t *w = G.map.work;
	COST(2);
	for (uint32_t row = 0; row < 16; row++) {
		for (uint32_t k = 0; k < 4; k++) {
			uint32_t o = row * 16 + 4 * k, p = ld32(src + 4 * k);
			uint32_t b = p & 0x08080808u, m = b - (b >> 3);
			st32(w + o, (ld32(w + o) & m) | (p & 0x07070707u));
		}
		src += TILE_PITCH;
		COST(40);
		POLL(0x40427e);
	}
	COST(1);
}

/* 0x404281: the composed cell to the screen: where the tile's bit 3 was
 * clear the composed pixel, where it was set the tile's own */
static void work_store(uint8_t *dst)
{
	const uint8_t *w = G.map.work;
	COST(2);
	for (uint32_t row = 0; row < 16; row++) {
		for (uint32_t k = 0; k < 4; k++) {
			uint32_t o = row * 16 + 4 * k;
			st32(dst + 4 * k, (ld32(w + o) & ld32(w + 0x200 + o)) | ld32(w + 0x100 + o));
		}
		dst += lk.pitch;
		COST(19);
		POLL(0x4042e4);
	}
	COST(1);
}

/* the sprite sheet pixels of sprite tile number t (+1) */
static const uint8_t *sprite_pixels(uint32_t t)
{
	return lk.sprites + (tile_offset((t - 1) & 0xfff) << 3);
}

/* 0x403eff: draw the cell onto the screen at offset dst_off of the locked
 * screen: the tile, and the 0-3 sprite tiles over it, the last drawn first. */
static void cell_draw(const struct cell *cell, uint32_t dst_off)
{
	uint8_t *dst = lk.screen + dst_off;
	const uint8_t *src = lk.tiles + (tile_offset(cell->tile & 0xfff) << 3);
	uint32_t layers = 0;
	COST(9 + 2 + 1); /* up to xor eax, cmp [edi+2], call */
	if (cell->spr[0] != 0) {
		layers = 1;
		COST(3);
		if (cell->spr[1] != 0) {
			layers = 2;
			COST(3);
			if (cell->spr[2] != 0) {
				layers = 3;
				COST(3);
			}
		}
	}
	switch (layers) {
	case 0:
		cell_tile(src, dst);
		break;
	case 1:
		MV_SET(layer_tile[0], cell->spr[0]);
		COST(4 + 6 + 2 + 1);
		work_load(src);
		work_overlay(sprite_pixels(MV(layer_tile[0])));
		work_store(dst);
		break;
	case 2:
		MV_SET(layer_tile[0], cell->spr[0]);
		MV_SET(layer_tile[1], cell->spr[1]);
		COST(7 + 12 + 3 + 1);
		work_load(src);
		work_overlay(sprite_pixels(MV(layer_tile[1])));
		work_overlay(sprite_pixels(MV(layer_tile[0])));
		work_store(dst);
		break;
	default:
		MV_SET(layer_tile[0], cell->spr[0]);
		MV_SET(layer_tile[1], cell->spr[1]);
		MV_SET(layer_tile[2], cell->spr[2]);
		COST(10 + 18 + 4 + 1);
		work_load(src);
		work_overlay(sprite_pixels(MV(layer_tile[2])));
		work_overlay(sprite_pixels(MV(layer_tile[1])));
		work_overlay(sprite_pixels(MV(layer_tile[0])));
		work_store(dst);
		break;
	}
	COST(2); /* popad, ret */
}

/* 0x4040f0: clear bit 3 of the 16 x 16 pixels at screen offset dst_off of the
 * locked screen */
static void cell_clear_bit3(uint32_t dst_off)
{
	uint8_t *dst = lk.screen + dst_off;
	COST(4);
	for (uint32_t row = 0; row < 16; row++) {
		for (uint32_t k = 0; k < 4; k++)
			st32(dst + 4 * k, ld32(dst + 4 * k) & 0x07070707u);
		dst += lk.pitch;
		COST(14);
		POLL(0x40412d);
	}
	COST(3);
}

/* ---- sub-op 4: the cells that changed */

/* 0x403d45: lock the three surfaces and draw the cells whose record
 * differs from the shadow, copying it there first; then unlock. Returns the
 * AX the last unlock leaves. */
uint32_t game_map_redraw_changed(void)
{
	lock_all();
	const struct map_rec *m = map_rec();
	uint32_t row_step = lk.pitch << 4;
	uint32_t dst_row = (m->screen_x << 4) + m->screen_y * lk.pitch;
	uint32_t vh = m->view_h, vw = m->view_w;
	const struct cell *c = cells();
	struct cell *sh = shadow();
	COST(12); /* after the locks: push ebp ... movzx ecx */
	for (uint32_t r = 0; r < vh; r++) {
		uint32_t dst = dst_row;
		COST(3);
		for (uint32_t k = 0; k < vw; k++) {
			if (c->tile != sh->tile) {
				COST(3);
			} else if (c->spr[0] != sh->spr[0]) {
				COST(6);
			} else if (c->spr[1] != sh->spr[1]) {
				COST(9);
			} else if (c->spr[2] != sh->spr[2]) {
				COST(12);
			} else {
				COST(12);
				goto next;
			}
			*sh = *c;
			COST(5);
			cell_draw(sh, dst);
next:
			c++;
			sh++;
			dst += 0x10;
			COST(4);
			POLL(0x403dfc);
		}
		dst_row += row_step;
		COST(4);
		POLL(0x403e02);
	}
	COST(1);
	uint32_t eax = unlock_all();
	COST(1);
	return eax;
}

/* ---- sub-op 5: every cell */

/* 0x403e30: lock the three surfaces, copy each cell record over its shadow,
 * clear bit 3 of its 16 x 16 screen pixels (0x4040f0) and draw it (0x403eff);
 * call the event check `between` (0x405216), which pumps messages, with the
 * surfaces still locked; then unlock. Returns the AX the last unlock leaves. */
uint32_t game_map_redraw_all(void (*between)(void))
{
	lock_all();
	const struct map_rec *m = map_rec();
	uint32_t row_step = lk.pitch << 4;
	uint32_t dst_row = (m->screen_x << 4) + m->screen_y * lk.pitch;
	uint32_t vh = m->view_h, vw = m->view_w;
	const struct cell *c = cells();
	struct cell *sh = shadow();
	COST(12); /* push ebp ... movzx ecx */
	for (uint32_t r = 0; r < vh; r++) {
		uint32_t dst = dst_row;
		COST(3);
		for (uint32_t k = 0; k < vw; k++) {
			*sh = *c;
			COST(4 + 1);
			cell_clear_bit3(dst);
			COST(1);
			cell_draw(sh, dst);
			c++;
			sh++;
			dst += 0x10;
			COST(4);
			POLL(0x403ec6);
		}
		dst_row += row_step;
		COST(4);
		POLL(0x403ecc);
	}
	COST(1); /* call 0x405216 */
	between();
	COST(1); /* pop ebp */
	uint32_t eax = unlock_all();
	COST(1);
	return eax;
}
