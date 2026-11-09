/*
 * The town map: typed views of its state and the internal interfaces of
 * src/game/map*.c. The state is G.map (cells, work planes, the variables
 * G.map.v) and G.seg1 (script segment 1). Semantics: re-notes.md "Town map".
 *
 * The map record M sits in segment 1 at the offset @[5] (G.save.n.map_sel);
 * the tile layer, the walk grid, the sprite table, the area table and the
 * entry table are further blocks of the same segment, found through offsets
 * in M. Offsets from scripts are not range checked (docs/refactor.md
 * "Out-of-range indices"): seg1_ptr() indexes G.seg1 by byte offset and
 * runs on as the native address arithmetic did.
 */
#ifndef NANPA2_GAME_MAP_H
#define NANPA2_GAME_MAP_H

#include <stdint.h>
#include <string.h>

#include "game/ctl.h"
#include "game/state.h"
#include "game/vm.h"
#include "x86/x86.h"

#pragma pack(push, 1)
/* the map state record M */
struct map_rec {
	uint16_t layer;    /* 0x00 offset of the tile layer block: u16 w, u16 h, w*h u16 tiles
	                    * (bit 15 = wall), then the walk grid (w*h bytes) */
	uint16_t min_x, min_y, max_x, max_y; /* 0x02 the view's scroll limits */
	uint16_t screen_x; /* 0x0a where the view sits on the screen: x in 16-pixel cells, */
	uint16_t screen_y; /* 0x0c y in pixels */
	uint16_t view_w, view_h; /* 0x0e size of the view in cells */
	uint16_t margin_l, margin_t, margin_r, margin_b; /* 0x12 scroll margins */
	uint16_t view_x, view_y; /* 0x1a the map cell at the view's top left */
	uint16_t map_w, map_h;   /* 0x1e copy of the layer's size */
	uint8_t pad_22[6];
	uint16_t patterns; /* 0x28 offset of the sprite pattern pointer table */
	uint16_t frames;   /* 0x2a animation frame table */
	uint16_t scripts;  /* 0x2c sprite script pointer table */
	uint16_t sprites;  /* 0x2e sprite table */
	uint16_t areas;    /* 0x30 area trigger table */
	uint16_t entries;  /* 0x32 entry (start position) table */
};

/* a sprite, 16 bytes in the sprite table, ended by a record whose flags are
 * 0xff. Sprite 0 is the player, 1 and 2 follow it. */
struct sprite {
	uint8_t flags;      /* 0x00 SPRITE_* */
	uint16_t x, y;      /* 0x01 map cell */
	uint8_t dest_x;     /* 0x05 where kind 13 walks to */
	uint8_t pattern;    /* 0x06 pattern set */
	uint8_t script;     /* 0x07 movement script */
	uint8_t script_pc;  /* 0x08 position in it */
	uint8_t pad_09;
	uint8_t frame;      /* 0x0a direction * 2 + step toggle (up, down, left, right) */
	uint8_t width;      /* 0x0b footprint in cells */
	uint8_t height;     /* 0x0c */
	uint8_t kind;       /* 0x0d movement kind */
	uint8_t count;      /* 0x0e steps left of that movement */
	uint8_t dest_y;     /* 0x0f */
};

/* an area trigger, 10 bytes, the table ended by x0 = 0xffff */
struct area {
	uint16_t x0, y0, x1, y1, code;
};

/* an entry, 18 bytes */
struct entry {
	uint16_t view_x, view_y, x, y, frame;
	uint16_t min_x, min_y, max_x, max_y;
};

/* a cell of the view: the layer's tile and up to three sprite tiles (+1; 0 = none) */
struct cell {
	uint16_t tile;
	uint16_t spr[3];
};

/* a record of the player's position history */
struct hist_rec {
	uint8_t frame;
	uint16_t x, y;
};
#pragma pack(pop)

_Static_assert(sizeof(struct map_rec) == 0x34, "map_rec");
_Static_assert(offsetof(struct map_rec, map_w) == 0x1e, "map_rec.map_w");
_Static_assert(offsetof(struct map_rec, patterns) == 0x28, "map_rec.patterns");
_Static_assert(sizeof(struct sprite) == 16, "sprite");
_Static_assert(offsetof(struct sprite, frame) == 0x0a && offsetof(struct sprite, dest_y) == 0x0f, "sprite");
_Static_assert(sizeof(struct area) == 10 && sizeof(struct entry) == 18, "area, entry");
_Static_assert(sizeof(struct cell) == 8 && sizeof(struct hist_rec) == 5, "cell, hist_rec");

enum {
	SPRITE_ACTIVE = 0x01, /* script steps it */
	SPRITE_DRAWN = 0x02,  /* its tiles are overlaid on the grid */
	SPRITE_SCROLL = 0x04, /* the view follows it and may scroll */
	SPRITE_SLIDE = 0x08,  /* bumps into walls do not slide it along them */
	SPRITE_GHOST = 0x10,  /* nothing collides with it */
	SPRITE_SOLID_NOT = 0x20,
	SPRITE_OFF = 0x80,
	SPRITE_END = 0xff,
	HISTORY_LEN = 10,
};

/* ---- views ---- */

/* the byte at offset off of script segment 1, n bytes; the segment runs on into
 * the memory after it (G, then the emulated memory) as it does natively */
static inline uint8_t *seg1_ptr(uint32_t off, uint32_t n)
{
	if (off + n <= SEG1_SIZE + IMG_SIZE_B + ANI_SIZE + 0x1000u)
		return G.seg1 + off;
	return x86_ptr(SEG1_BASE + off, n);
}
static inline uint32_t seg1_u8(uint32_t off) { return *seg1_ptr(off, 1); }
static inline uint32_t seg1_u16(uint32_t off) { uint16_t v; memcpy(&v, seg1_ptr(off, 2), 2); return v; }
static inline void seg1_set8(uint32_t off, uint32_t v) { *seg1_ptr(off, 1) = (uint8_t)v; }
static inline void seg1_set16(uint32_t off, uint32_t v) { uint16_t t = (uint16_t)v; memcpy(seg1_ptr(off, 2), &t, 2); }

static inline struct map_rec *map_rec(void)
{
	return (struct map_rec *)seg1_ptr(SV(map_sel), sizeof(struct map_rec));
}
static inline struct sprite *sprite_at(uint32_t off)
{
	return (struct sprite *)seg1_ptr(off, sizeof(struct sprite));
}
static inline struct area *area_at(uint32_t off)
{
	return (struct area *)seg1_ptr(off, sizeof(struct area));
}
static inline struct entry *entry_at(uint32_t off)
{
	return (struct entry *)seg1_ptr(off, sizeof(struct entry));
}
/* the sprite table of M, sprite 0 first */
static inline struct sprite *sprites_of(const struct map_rec *m)
{
	return sprite_at(m->sprites);
}

/* the cell grid and its shadow (what was drawn last); a view larger than the
 * array runs on into the shadow as natively */
static inline struct cell *cells(void) { return (struct cell *)G.map.cells; }
static inline struct cell *shadow(void) { return (struct cell *)G.map.shadow; }

/* the history ring G.map.v.history; a record number past 10 runs on into the
 * padding after it */
static inline struct hist_rec *hist(uint32_t i)
{
	return (struct hist_rec *)((uint8_t *)&G.map.v + offsetof(struct map_vars, history) + 5 * i);
}

/* the tile offset table (read-only): tile number to the block's offset in eighths
 * in the sheet; a number past 1000 runs on into the history ring */
static inline uint32_t tile_offset(uint32_t i)
{
	uint16_t v;
	memcpy(&v, (uint8_t *)G.map.v.tile_offsets + 2 * i, 2);
	return v;
}

/* the result of the map's message pump (0x40609c): BX = BL | BH << 8 with
 * BL bit 0 the left button (Enter / click), bit 1 the right (Esc / click),
 * BH = the right button << 1; the mouse position */
struct pump_state {
	uint32_t bx;
	uint32_t x, y;
};

/* ---- map.c: sub-ops and the area / collision tests ---- */
uint32_t game_op_map(void);                /* 0x4073ee, the 04 26 handler (map_pump.c) */
uint32_t game_map_op(uint32_t op);         /* 0x403a70: the sub-ops that do not wait; the AX they leave */
void game_map_load_args(void);             /* 0x403a70: arguments 1..6 to G.map.v.args */
int game_map_op_in_c(uint32_t op);
uint32_t game_map_area_trigger(uint32_t n);  /* 0x404f8c: AX */
uint32_t game_map_collision(uint32_t n);     /* 0x40500e: AX */
int game_map_collide(const struct sprite *s); /* 0x404e25: the index of the sprite in the way or -1 */

/* ---- map_move.c: the movers, AL = 0xff moved, 0 blocked ---- */
int game_map_move_up(struct sprite *s);    /* 0x404731 */
int game_map_move_down(struct sprite *s);  /* 0x404937 */
int game_map_move_left(struct sprite *s);  /* 0x404a9a */
int game_map_move_right(struct sprite *s); /* 0x404c9f */

/* ---- map_path.c: walking to a place ---- */
void game_map_plan_walk(struct sprite *s, uint32_t gx, uint32_t gy); /* 0x405966 */
/* 0x405691: one step; *al = the last mover's result when one ran */
void game_map_walk_step(struct sprite *s, uint32_t *eax);
uint32_t game_map_op_walk(void);           /* 0x405542, sub-op 30: AX */

/* ---- map_draw.c: the cell grid and the drawing ---- */
uint32_t game_map_fill_grid(uint32_t eax);       /* 0x403b28, sub-op 2: the AX it leaves (eax in) */
uint32_t game_map_overlay_sprites(uint32_t eax); /* 0x403bd2, sub-op 3 */
uint32_t game_map_redraw_changed(void);          /* 0x403d45, sub-op 4: the AX of the last unlock */
uint32_t game_map_redraw_all(void (*between)(void)); /* 0x403e30, sub-op 5: calls between() (the
                                                          * event check) with the surfaces locked */

/* ---- map_sprite.c: the sprite scripts ---- */
/* 0x40431b: one sprite's step; *eax is the AX leftover of the sprite loop */
void game_map_sprite_step(struct sprite *s, uint32_t *eax);

/* ---- map_pump.c ---- */
struct pump_state game_map_pump(void);                  /* 0x40609c */
void game_map_player_step(struct sprite *s, uint32_t *eax); /* 0x404450 */
uint32_t game_map_event_check(void);                    /* 0x405216: AX */

#endif
