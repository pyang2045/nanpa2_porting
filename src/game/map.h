/*
 * The town map (stage 5A): state layout and the internal interfaces of
 * src/game/map*.c. Emulated memory stays the only state store, so the
 * native code that is left (the parts that pump messages) sees what the C
 * does. Semantics: re-notes.md "Town map".
 *
 * The map state record M sits in script segment 1 at the word @[5]
 * (0x4452d8); the tile layer, the walk grid, the sprite table, the area
 * table and the entry table are further blocks of the same segment, found
 * through offsets in M. All offsets below are bytes from the start of the
 * record or block they belong to.
 */
#ifndef NANPA2_GAME_MAP_H
#define NANPA2_GAME_MAP_H

#include <stdint.h>

#include "game/vm.h"
#include "x86/x86.h"

#define SEG1 0x4462beu       /* script segment 1: the map data lives here */
#define MAP_SEL 0x4452d8u    /* u16 @[5]: offset of M in segment 1 */
#define MAP_ARGS 0x43555eu   /* u16[6]: arguments 1..6 of the 04 26 sub-op */
#define MAP_WALK_BASE 0x435574u /* u16: segment offset of the walk grid */
#define MAP_AUTOWALK 0x435573u  /* u8: 0xff while the player follows the walk grid */
#define MAP_MOVED 0x435571u     /* u8: 0xff when a mover moved the player this frame */
#define MAP_DIR 0x435570u       /* u8: facing of the sprite being tested (frame >> 1) */
#define MAP_LAST_SPRITE 0x43556au /* u16: event state (see map_event.c) */
#define MAP_LAST_CLICK 0x43556cu
#define MAP_LAST_AREA 0x43556eu

/* the map state record M (u16 unless noted) */
enum {
	M_LAYER = 0x00,    /* offset of the tile layer block: u16 w, u16 h, then w*h u16 tiles,
	                    * then the walk grid (w*h bytes) */
	M_MIN_X = 0x02,    /* the view cannot scroll left / up of these, nor right / down of */
	M_MIN_Y = 0x04,
	M_MAX_X = 0x06,    /* the last visible column / row the view may show */
	M_MAX_Y = 0x08,
	M_SCREEN_X = 0x0a, /* where the view sits on the screen: x in 16-pixel cells, */
	M_SCREEN_Y = 0x0c, /* y in pixels */
	M_VIEW_W = 0x0e,   /* size of the view in cells */
	M_VIEW_H = 0x10,
	M_MARGIN_L = 0x12, /* cells between the view edge and the player where scrolling starts */
	M_MARGIN_T = 0x14,
	M_MARGIN_R = 0x16,
	M_MARGIN_B = 0x18,
	M_VIEW_X = 0x1a,   /* the map cell at the view's top left */
	M_VIEW_Y = 0x1c,
	M_MAP_W = 0x1e,    /* copy of the layer's size */
	M_MAP_H = 0x20,
	M_PATTERNS = 0x28, /* offset of the sprite pattern pointer table */
	M_FRAMES = 0x2a,   /* offset of the animation frame table */
	M_SCRIPTS = 0x2c,  /* offset of the sprite script pointer table */
	M_SPRITES = 0x2e,  /* offset of the sprite table */
	M_AREAS = 0x30,    /* offset of the area trigger table */
	M_ENTRIES = 0x32,  /* offset of the entry (start position) table */
};

/* a sprite: 16 bytes in the sprite table, ended by a record whose flags
 * are 0xff. Sprite 0 is the player, 1 and 2 follow it. */
enum {
	S_FLAGS = 0x00,   /* u8: SPRITE_* */
	S_X = 0x01,       /* u16: map cell */
	S_Y = 0x03,
	S_DEST_X = 0x05,  /* u8: where kind 13 walks to */
	S_PATTERN = 0x06, /* u8: pattern set */
	S_SCRIPT = 0x07,  /* u8: movement script */
	S_SCRIPT_PC = 0x08, /* u8: position in it */
	S_FRAME = 0x0a,   /* u8: direction * 2 + step toggle (up, down, left, right) */
	S_WIDTH = 0x0b,   /* u8: footprint in cells */
	S_HEIGHT = 0x0c,
	S_KIND = 0x0d,    /* u8: movement kind */
	S_COUNT = 0x0e,   /* u8: steps left of that movement */
	S_DEST_Y = 0x0f,  /* u8 */
	SPRITE_SIZE = 16,
};
enum {
	SPRITE_ACTIVE = 0x01, /* script steps it */
	SPRITE_DRAWN = 0x02,  /* its tiles are overlaid on the grid */
	SPRITE_SCROLL = 0x04, /* the view follows it and may scroll */
	SPRITE_SLIDE = 0x08,  /* bumps into walls do not slide it along them */
	SPRITE_GHOST = 0x10,  /* nothing collides with it */
	SPRITE_SOLID_NOT = 0x20,
	SPRITE_OFF = 0x80,
	SPRITE_END = 0xff,
};

/* the position history ring: 10 records of {frame u8, x u16, y u16}, the
 * player's last positions, which the follower sprites replay */
#define HISTORY 0x435dcau
#define HISTORY_LEN 10

static inline uint32_t map_base(void)
{
	return SEG1 + rd16(MAP_SEL);
}

/* ---- map.c: sub-ops and the area / collision tests ---- */
void game_map_dispatch(void);        /* 0x403a70 */
void game_map_dispatch_args(void);   /* 0x403a70 up to the call of the sub-op */
int game_map_op_in_c(uint32_t op);
void game_map_area_trigger(void);    /* 0x404f8c */
void game_map_collision(void);       /* 0x40500e */

int game_map_collide(uint32_t s);     /* 0x404e25 */

/* ---- map_move.c: the movers ---- */
void game_map_move_up(void);          /* 0x404731 */
void game_map_move_down(void);        /* 0x404937 */
void game_map_move_left(void);        /* 0x404a9a */
void game_map_move_right(void);       /* 0x404c9f */

/* ---- map_sprite.c: the sprite scripts ---- */
int game_map_sprite_step(void);       /* 0x40431b (and 0x404410, the kind stubs); 1 = the player's */

/* ---- map_path.c: walking to a place ---- */
void game_map_plan_walk(void);        /* 0x405966 */
void game_map_walk_step(void);        /* 0x405691 */
uint32_t game_map_op_walk(void);      /* 0x405542, sub-op 30 */

/* ---- map_draw.c: the cell grid and the drawing ---- */
void game_map_fill_grid(void);        /* 0x403b28, sub-op 2 */
void game_map_overlay_sprites(void);  /* 0x403bd2, sub-op 3 */
void game_map_redraw_changed(void);   /* 0x403d45, sub-op 4 */
void game_map_redraw_all_begin(void); /* 0x403e30 up to its call of 0x405216, sub-op 5 */
void game_map_redraw_all_end(void);   /* 0x403ed3 */
void game_map_cell_draw(void);        /* 0x403eff */
void game_map_cell_clear_bit3(void);  /* 0x4040f0 */

/* ---- map_pump.c: the chain that pumps messages (pieces, jump units) ---- */
uint32_t game_op_map(void);           /* 0x4073ee, the 04 26 handler */
uint32_t game_op_map_done(void);      /* 0x407410 */
uint32_t game_pump(void);             /* 0x40609c */
uint32_t game_pump_done(void);        /* 0x4060a2 */
uint32_t game_pl_pumped_a(void);      /* 0x40448b */
uint32_t game_pl_pumped_b(void);      /* 0x4044a9 */
uint32_t game_pl_click(void);         /* 0x4044b7 */
uint32_t game_pl_keys(void);          /* 0x4045eb */
uint32_t game_s12_pumped_a(void);     /* 0x4051e3 */
uint32_t game_s12_released(void);     /* 0x40520f */
uint32_t game_ev_pumped_a(void);      /* 0x405233 */
uint32_t game_ev_pumped_b(void);      /* 0x405273 */
uint32_t game_ev_released(void);      /* 0x40539f */
uint32_t game_rc_pumped(void);        /* 0x40559d */
int game_chain_words(void);

/* ---- map_fuzz.c: random states for the differential test (NANPA2_FUZZ) ---- */
void (*game_map_fuzz_for(uint32_t addr))(uint32_t *);
extern int game_map_fuzz_skip;

#endif
