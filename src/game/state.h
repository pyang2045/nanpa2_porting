/*
 * The game state as C structs (docs/refactor.md, R1).
 *
 * The game state is one object, G: the whole data area of nanpa2.exe that the game
 * writes (0x431000-0x4c0000) in its native layout, so that the unchecked indices of
 * the scripts land where they did natively, and the FLAG file and the checkpoints
 * see the bytes they always did. The comments give the native address of each field,
 * and an address is also an index into G (game/gmem.h). Code names the field. G is
 * initialised from the asset (state_init) and is the only game state.
 */
#ifndef NANPA2_GAME_STATE_H
#define NANPA2_GAME_STATE_H

#include <stddef.h>
#include <stdint.h>

/* The layout of G is the file format of the saves and of the asset: little-endian throughout */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the game state is little-endian (the FLAG files and the data image are)"
#endif


#pragma pack(push, 1)

/* The 0xc00 bytes of a FLAG file (script op 04 21 saves and loads exactly
 * these, 0x401160 / 0x401050): the layout is the file format, so a player's
 * existing saves load, and fields sit at their file offsets (checked below).
 * Segment 0 of the script VM reaches it by offset (register '@' is the offset
 * of w[0]) so scripts address it as words. */
#define SAVE_BASE 0x444e78u
#define SAVE_SIZE 0xc00u
#define SAVE_FLAG_BYTES 0x400u
#define SAVE_REGS 27u
#define SAVE_ARR 0x4452ceu /* address of @[0] */
struct save_image {
	char name[32];                /* 0x444e78 name of the running MES file */
	uint8_t flags[SAVE_FLAG_BYTES]; /* 0x444e98 4-bit flags, two per byte (even n low) */
	uint16_t reg[SAVE_REGS];      /* 0x445298 registers '@' to 'Z' */
	union {
		uint16_t w[(SAVE_SIZE - 0x456) / 2]; /* 0x4452ce @[i]: the word array */
		struct {
			uint16_t w0_4[5];     /* 0x4452ce @[0..4]; @[0] the work buffer's offset */
			uint16_t map_sel;     /* 0x4452d8 @[5] offset of the map record in segment 1 */
			uint16_t surface_select; /* 0x4452da @[6] surface the text window, rect ops and pictures use */
			uint16_t segment_select; /* 0x4452dc @[7] */
			uint16_t text_delay;  /* 0x4452de @[8] frames (x4) after each character */
			uint8_t vflags;       /* 0x4452e0 @[9] low byte: bit 0 keep palette on picture load,
					       * 1 stop the per-frame step, 2 click handlers off, 4 keep the
					       * palette tables, 5-6 survive a FLAG load */
			uint8_t input_enable; /* 0x4452e1 menu inputs (bit k + 1 for input k) */
			uint16_t mouse_x;     /* 0x4452e2 */
			uint16_t mouse_y;     /* 0x4452e4 0-399 */
			uint16_t w_e6;        /* 0x4452e6 */
			uint16_t text_left;   /* 0x4452e8 text window, columns */
			uint16_t text_top;    /* 0x4452ea rows */
			uint16_t text_right;  /* 0x4452ec */
			uint16_t text_bottom; /* 0x4452ee */
			uint16_t text_x;      /* 0x4452f0 cursor column */
			uint16_t text_y;      /* 0x4452f2 cursor row */
			uint16_t text_colour; /* 0x4452f4 low nibble fg, next bg */
			uint8_t text_size;    /* 0x4452f6 Big5 font size */
			uint8_t number_format; /* 0x4452f7 low nibble digits, high nibble ASCII */
			uint8_t text_line;    /* 0x4452f8 line height in rows */
			uint8_t text_advance; /* 0x4452f9 character width in columns */
			uint16_t menu_list_ptr; /* 0x4452fa @[22] offset of the menu item list */
			uint16_t w_fc;        /* 0x4452fc */
			uint8_t menu_skip;    /* 0x4452fe visible items to skip */
			uint8_t b_ff;         /* 0x4452ff */
			uint16_t menu_rows;   /* 0x445300 */
			uint16_t menu_count;  /* 0x445302 */
			uint16_t menu_run;    /* 0x445304 the menu loop runs while set */
			uint8_t pad_306[6];   /* 0x445306 */
			uint16_t palette[16]; /* 0x44530c the base colours, 0x0GRB */
			uint8_t pad_32c[16];  /* 0x44532c */
			uint32_t segment[4];  /* 0x44533c script segment base addresses */
			uint8_t pad_34c[62];  /* 0x44534c */
			uint16_t pic_rect[4]; /* 0x44538a last picture: x / 8, y, w / 8, h */
			uint8_t pad_392[4];   /* 0x445392 */
			uint8_t work[0x445a78 - 0x445396]; /* 0x445396 @[0]: work buffer */
		} n;
	};
};
#pragma pack(pop)

#define SAVE_CHECK(field, addr) _Static_assert(offsetof(struct save_image, field) == (addr) - SAVE_BASE, #field)
SAVE_CHECK(flags, 0x444e98u);
SAVE_CHECK(reg, 0x445298u);
SAVE_CHECK(w, SAVE_ARR);
SAVE_CHECK(n.map_sel, 0x4452d8u);
SAVE_CHECK(n.surface_select, 0x4452dau);
SAVE_CHECK(n.segment_select, 0x4452dcu);
SAVE_CHECK(n.text_delay, 0x4452deu);
SAVE_CHECK(n.vflags, 0x4452e0u);
SAVE_CHECK(n.input_enable, 0x4452e1u);
SAVE_CHECK(n.mouse_x, 0x4452e2u);
SAVE_CHECK(n.mouse_y, 0x4452e4u);
SAVE_CHECK(n.text_left, 0x4452e8u);
SAVE_CHECK(n.text_bottom, 0x4452eeu);
SAVE_CHECK(n.text_x, 0x4452f0u);
SAVE_CHECK(n.text_colour, 0x4452f4u);
SAVE_CHECK(n.text_size, 0x4452f6u);
SAVE_CHECK(n.text_advance, 0x4452f9u);
SAVE_CHECK(n.menu_list_ptr, 0x4452fau);
SAVE_CHECK(n.menu_skip, 0x4452feu);
SAVE_CHECK(n.menu_rows, 0x445300u);
SAVE_CHECK(n.menu_run, 0x445304u);
SAVE_CHECK(n.palette, 0x44530cu);
SAVE_CHECK(n.segment, 0x44533cu);
SAVE_CHECK(n.pic_rect, 0x44538au);
SAVE_CHECK(n.work, 0x445396u);
_Static_assert(sizeof(struct save_image) == SAVE_SIZE, "the save image is 0xc00 bytes");

/* The script VM's runtime state and the timing and surface words next to it,
 * 0x445cdc-0x4462be (not saved). */
#define RT_BASE 0x445cdcu
#define RT_LABELS 60u
#pragma pack(push, 1)
struct vm_rt {
	uint32_t pad_cdc;          /* 0x445cdc the native top-level VM stack pointer: dead since R3, outside the FLAG image */
	uint8_t cursor_shown;      /* 0x445ce0 the game cursor is drawn (ext 15) */
	uint8_t click_busy;        /* 0x445ce1 a click handler's block is running */
	uint16_t click_handler[2]; /* 0x445ce2 blocks for the left and right button (ext 20) */
	uint8_t number_text[16];   /* 0x445ce6 digits op 15 writes */
	uint8_t arg_type[16];      /* 0x445cf6 argument types of the last list */
	uint16_t arg_value[16];    /* 0x445d06 their values */
	uint8_t string_buf[32];    /* 0x445d26 file names for ext 1c, 1d, 25 */
	uint8_t saved_name[32];    /* 0x445d46 the running file's name during a nested call (op 1f) */
	uint8_t name_keep[32];     /* 0x445d66 the name kept over a FLAG load */
	char flag_name[9];         /* 0x445d86 "FLAGn" (n at 0x445d8a) */
	uint8_t input_bits;        /* 0x445d8f arrows 1 2 4 8, Enter / click 0x10, Esc / right click 0x20, ctrl 0x40, end 0x80 */
	uint8_t menu_list[44];     /* 0x445d90 menu items {text offset, number} */
	uint16_t label[RT_LABELS]; /* 0x445dbc labels (op 12 sets, 13 calls; 32 + k the menu inputs, 38 and 39 the menu) */
	uint8_t pad_e34[2];        /* 0x445e34 */
	uint16_t saved_label[RT_LABELS]; /* 0x445e36 the label table during a nested call */
	uint16_t palette_picture[240]; /* 0x445eae the picture's colours, 5:5:5 */
	uint16_t palette_saved[256];   /* 0x44608e both tables saved (0x40756c) */
	uint32_t fade_steps;       /* 0x44628e palette fade length */
	uint32_t frame;            /* 0x446292 frames since start (the 60 Hz timer) */
	uint32_t stopwatch;        /* 0x446296 frame of the last op 19 2 x 0 */
	uint32_t store_segment;    /* 0x44629a segment op 0d stores through */
	uint32_t surface[4];       /* 0x44629e platform surface handles (platform/surface.h), 0 the screen */
	uint32_t pitch[4];         /* 0x4462ae their pitches */
};
#pragma pack(pop)

#define RT_CHECK(field, addr) _Static_assert(offsetof(struct vm_rt, field) == (addr) - RT_BASE, #field)
RT_CHECK(cursor_shown, 0x445ce0u);
RT_CHECK(click_handler, 0x445ce2u);
RT_CHECK(number_text, 0x445ce6u);
RT_CHECK(arg_type, 0x445cf6u);
RT_CHECK(arg_value, 0x445d06u);
RT_CHECK(string_buf, 0x445d26u);
RT_CHECK(saved_name, 0x445d46u);
RT_CHECK(name_keep, 0x445d66u);
RT_CHECK(flag_name, 0x445d86u);
RT_CHECK(input_bits, 0x445d8fu);
RT_CHECK(menu_list, 0x445d90u);
RT_CHECK(label, 0x445dbcu);
RT_CHECK(saved_label, 0x445e36u);
RT_CHECK(palette_picture, 0x445eaeu);
RT_CHECK(palette_saved, 0x44608eu);
RT_CHECK(fade_steps, 0x44628eu);
RT_CHECK(frame, 0x446292u);
RT_CHECK(stopwatch, 0x446296u);
RT_CHECK(store_segment, 0x44629au);
RT_CHECK(surface, 0x44629eu);
RT_CHECK(pitch, 0x4462aeu);
_Static_assert(sizeof(struct vm_rt) == 0x4462beu - RT_BASE, "vm_rt covers 0x445cdc-0x4462be");

/* Palette, fade, cursor, window and DirectDraw globals, 0x4aac50-0x4ac51c. The
 * timer tick (0x4028c0, src/game/timer.c) steps the fade and blinks the cursor
 * from here. */
#define SC_BASE 0x4aac50u
#pragma pack(push, 1)
struct screen_state {
	uint32_t cursor_saved_y;   /* 0x4aac50 where the cursor sprite's background was saved */
	uint32_t cursor_saved_x;   /* 0x4aac54 */
	uint8_t pal_start[1024];   /* 0x4aac58 the palette entries as the fade started (RGBA x 256) */
	uint32_t scratch_surface;  /* 0x4ab058 the 640x480 scratch surface (text): a surface handle */
	uint32_t hwnd;             /* 0x4ab05c the window: unused since R4 (kept for the layout) */
	int32_t pal_delta[256][3]; /* 0x4ab060 target - displayed, per channel */
	uint8_t cursor_saved[1024]; /* 0x4abc60 background under the cursor, 32 rows of 32 bytes */
	uint32_t cursor_y;         /* 0x4ac060 */
	uint32_t cursor_x;         /* 0x4ac064 */
	uint32_t letterbox;        /* 0x4ac068 1: the 640x480 image sits 40 lines down */
	uint32_t hinstance;        /* 0x4ac06c unused since R4 */
	uint32_t font[3];          /* 0x4ac070 font handles by size (platform/text.h) */
	uint8_t pal_entries[1024]; /* 0x4ac07c PALETTEENTRY[256]: red, green, blue, flags */
	uint8_t pad_47c[4];        /* 0x4ac47c */
	uint8_t surface_desc[0x24]; /* 0x4ac480 DDSURFACEDESC of the last Lock: unused since R4 ... */
	uint32_t lock_ptr;         /* 0x4ac4a4 ... its lpSurface: unused since R4 (a lock returns a pointer) */
	uint8_t surface_desc_rest[0x44]; /* 0x4ac4a8 */
	uint32_t active;           /* 0x4ac4ec the timer runs the game only while set (WM_ACTIVATEAPP) */
	uint32_t ddraw;            /* 0x4ac4f0 unused since R4 (the DirectDraw object) */
	uint32_t palette_obj;      /* 0x4ac4f4 unused since R4 (the DirectDraw palette object) */
	uint32_t logo;             /* 0x4ac4f8 the logo movie is playing */
	uint32_t pad_4fc;          /* 0x4ac4fc */
	uint32_t timer_id;         /* 0x4ac500 unused since R4 (the timer handle) */
	uint32_t fade_active;      /* 0x4ac504 a palette fade is running */
	uint32_t fade_step;        /* 0x4ac508 of 16 */
	uint32_t fade_wait;        /* 0x4ac50c ticks since the last step */
	uint32_t cursor_on;        /* 0x4ac510 the software cursor blinks */
	uint32_t cursor_frame;     /* 0x4ac514 */
	uint32_t paused;           /* 0x4ac518 */
};
#pragma pack(pop)
#define SC_CHECK(field, addr) _Static_assert(offsetof(struct screen_state, field) == (addr) - SC_BASE, #field)
SC_CHECK(pal_start, 0x4aac58u);
SC_CHECK(scratch_surface, 0x4ab058u);
SC_CHECK(pal_delta, 0x4ab060u);
SC_CHECK(cursor_saved, 0x4abc60u);
SC_CHECK(cursor_y, 0x4ac060u);
SC_CHECK(font, 0x4ac070u);
SC_CHECK(pal_entries, 0x4ac07cu);
SC_CHECK(surface_desc, 0x4ac480u);
SC_CHECK(lock_ptr, 0x4ac4a4u);
SC_CHECK(active, 0x4ac4ecu);
SC_CHECK(timer_id, 0x4ac500u);
SC_CHECK(fade_active, 0x4ac504u);
SC_CHECK(cursor_on, 0x4ac510u);
SC_CHECK(paused, 0x4ac518u);

_Static_assert(sizeof(struct screen_state) == 0x4ac51cu - SC_BASE, "screen_state covers 0x4aac50-0x4ac51c");

/* The ten animation slots (script and loop state of the ANI interpreter,
 * anim.c) and the words after them, 0x4312cc-0x4313c6. A slot's number comes
 * from the script and is not range checked natively, so anim.c addresses the
 * slots (an index past 9 runs on into last_frame and idle as the native code
 * does) and names only the fixed words. */
#define AN_BASE 0x4312ccu
#define AN_SLOTS 10u
#pragma pack(push, 1)
struct anim_slot {
	uint8_t state;     /* +0 SLOT_STOPPED 0, SLOT_TO_MARK 1, SLOT_RUNNING 0xff */
	uint8_t delay;     /* +1 frames to wait before the next instruction */
	uint8_t count[2];  /* +2 loop counters A, B */
	uint32_t start;    /* +4 the script's first instruction (an address in the ANI file) */
	uint32_t pc;       /* +8 the next instruction */
	uint32_t loop[2];  /* +12 loop starts */
	uint16_t x, y;     /* +20 set by sub-op 5, never read */
};
struct anim_state {
	struct anim_slot slot[AN_SLOTS]; /* 0x4312cc */
	uint32_t last_frame;             /* 0x4313bc the frame the slots last ran */
	uint32_t idle;                   /* 0x4313c0 script start of a reset slot */
	uint16_t random_seed;            /* 0x4313c4 seed of the script random number generator */
};
#pragma pack(pop)
_Static_assert(sizeof(struct anim_slot) == 24, "slots are 24 bytes");
_Static_assert(sizeof(struct anim_state) == 0x4313c6u - AN_BASE, "anim_state covers 0x4312cc-0x4313c6");
_Static_assert(offsetof(struct anim_state, last_frame) == 0x4313bcu - AN_BASE, "last_frame");
_Static_assert(offsetof(struct anim_state, random_seed) == 0x4313c4u - AN_BASE, "random_seed");

/* The town map's state outside the script segments, 0x4313c6-0x435e78: the cell
 * grid and its shadow, the drawing work planes, the locked surface pointers, the
 * 04 26 arguments, the movers' flags, the sub-op jump table, the tile offset table
 * (read-only), the player's position history and the path planner's words
 * (src/game/map*.c, re-notes.md "Town map"). */
#define MAPD_BASE 0x4313c6u
#define MAPD_CELLS 0x1f40u
#pragma pack(push, 1)
struct map_vars {
	uint32_t locked_screen;    /* 0x435548 the pixel pointers of the three locked surfaces: unused since R4 (map_draw.c holds them) */
	uint32_t locked_tiles;     /* 0x43554c */
	uint32_t locked_sprites;   /* 0x435550 */
	uint8_t pad_554[2];        /* 0x435554 */
	uint16_t layer_tile[3];    /* 0x435556 the sprite tiles of the cell being drawn */
	uint8_t draw_w;            /* 0x43555c */
	uint8_t draw_h;            /* 0x43555d */
	uint16_t args[6];          /* 0x43555e arguments 1..6 of the 04 26 sub-op */
	uint16_t last_sprite;      /* 0x43556a event state */
	uint16_t last_click;       /* 0x43556c */
	uint16_t last_area;        /* 0x43556e */
	uint8_t dir;               /* 0x435570 facing of the sprite being tested (frame >> 1) */
	uint8_t moved;             /* 0x435571 0xff when a mover moved the player this frame */
	uint8_t scrolled;          /* 0x435572 the view scrolled in the player's step */
	uint8_t autowalk;          /* 0x435573 0xff while the player follows the walk grid */
	uint16_t walk_base;        /* 0x435574 segment offset of the walk grid */
	uint32_t subop[33];        /* 0x435576 sub-op handler table (native addresses) */
	uint16_t tile_offsets[1000]; /* 0x4355fa tile number -> offset in the tile sheet (read-only) */
	uint8_t history[50];       /* 0x435dca ring of 10 {frame u8, x u16, y u16} */
	uint8_t pad_dfc[0x64];     /* 0x435dfc */
	uint8_t random_seed;       /* 0x435e60 state of the sprite kind 0xe generator */
	uint8_t approach;          /* 0x435e61 bit 0: walk up to a goal that is a wall */
	uint16_t path_grid;        /* 0x435e62 copy of the walk grid's offset (written, never read) */
	uint16_t path_from_cell;   /* 0x435e64 */
	uint16_t path_goal_cell;   /* 0x435e66 */
	uint16_t path_goal_x;      /* 0x435e68 */
	uint16_t path_goal_y;      /* 0x435e6a */
	uint16_t path_from_x;      /* 0x435e6c */
	uint16_t path_from_y;      /* 0x435e6e */
	uint16_t path_w;           /* 0x435e70 */
	uint16_t path_h;           /* 0x435e72 */
	uint8_t pad_e74[4];        /* 0x435e74 */
};
struct map_data {
	uint8_t pad_3c6[2];                 /* 0x4313c6 */
	uint8_t cells[MAPD_CELLS];          /* 0x4313c8 4 words per cell */
	uint8_t shadow[MAPD_CELLS];         /* 0x433308 what was drawn last */
	uint8_t work[0x300];                /* 0x435248 3 planes of 256 bytes */
	struct map_vars v;                  /* 0x435548 */
};
#pragma pack(pop)
_Static_assert(offsetof(struct map_vars, args) == 0x43555eu - 0x435548u, "args");
_Static_assert(offsetof(struct map_vars, walk_base) == 0x435574u - 0x435548u, "walk_base");
_Static_assert(offsetof(struct map_vars, subop) == 0x435576u - 0x435548u, "subop");
_Static_assert(offsetof(struct map_vars, tile_offsets) == 0x4355fau - 0x435548u, "tile_offsets");
_Static_assert(offsetof(struct map_vars, history) == 0x435dcau - 0x435548u, "history");
_Static_assert(offsetof(struct map_vars, random_seed) == 0x435e60u - 0x435548u, "random_seed");
_Static_assert(offsetof(struct map_vars, path_h) == 0x435e72u - 0x435548u, "path_h");
_Static_assert(sizeof(struct map_vars) == 0x435e78u - 0x435548u, "map_vars");
_Static_assert(sizeof(struct map_data) == 0x435e78u - MAPD_BASE, "map_data");

/* The script and picture memory the VM addresses by segment and offset: segment 0
 * is the script image (it runs on through the save image and the VM block),
 * segment 1 (0x4462be) the map data, segments 2 and 3 (0x4562be) the picture
 * buffers. The objects are contiguous as natively, so an offset past the end of a
 * segment runs into the next, as it does there. */
#define SCRIPT_BASE 0x435e78u
#define SCRIPT_SIZE 0xf000u
#define SEG1_BASE 0x4462beu
#define SEG1_SIZE 0x10000u
#define IMG_BASE 0x4562beu
#define IMG_SIZE_B 0x50000u
#define ANI_BASE 0x4a62beu
#define ANI_SIZE 0x4992u

/* The sound words the native DirectSound code kept (src/platform/sound.c replaces it;
 * nothing in C reads them) and the cursor bitmap */
#define SND_BASE 0x4be540u
#define CBITS_BASE 0x4be900u
#pragma pack(push, 1)
/* The whole initialised and zero-initialised data of nanpa2.exe that the game
 * writes, 0x431000-0x4c0000, in its native layout (so every address the native
 * code computed, including the unchecked indices of the script, lands where it
 * did). The rest of the exe (code, read-only data, the C runtime) is not part of the
 * engine: nothing reads it. */
#define DATA_BASE 0x431000u
#define DATA_END 0x4c0000u
struct game_state {
	uint8_t head[0x4312ccu - DATA_BASE]; /* 0x431000 .data head: file name formats and strings
	                                      * (read-only) and the CD prefix at 0x431030 */
	struct anim_state an;                /* 0x4312cc */
	struct map_data map;                 /* 0x4313c6 */
	uint8_t script[SCRIPT_SIZE];         /* 0x435e78 MES image, segment 0 */
	struct save_image save;              /* 0x444e78 */
	uint8_t pad_a78[RT_BASE - 0x445a78u]; /* 0x445a78 */
	struct vm_rt rt;                     /* 0x445cdc */
	uint8_t seg1[SEG1_SIZE];             /* 0x4462be segment 1: map data */
	uint8_t img[IMG_SIZE_B];             /* 0x4562be segments 2 and 3: picture buffers */
	uint8_t ani[ANI_SIZE];               /* 0x4a62be the ANI file */
	struct screen_state sc;              /* 0x4aac50 */
	uint8_t pad_51c[SND_BASE - 0x4ac51cu]; /* 0x4ac51c CRT and MP3 library data */
	uint8_t snd[CBITS_BASE - SND_BASE];  /* 0x4be540 the native sound words and buffers */
	uint8_t cursor_bits[0x200];          /* 0x4be900 32x32 AND/XOR cursor bitmap */
	uint8_t pad_b00[DATA_END - 0x4beb00u]; /* 0x4beb00 */
};
#pragma pack(pop)
#define GS_CHECK(field, addr) _Static_assert(offsetof(struct game_state, field) == (addr) - DATA_BASE, #field)
GS_CHECK(an, 0x4312ccu);
GS_CHECK(map, MAPD_BASE);
GS_CHECK(script, SCRIPT_BASE);
GS_CHECK(save, SAVE_BASE);
GS_CHECK(rt, RT_BASE);
GS_CHECK(seg1, SEG1_BASE);
GS_CHECK(img, IMG_BASE);
GS_CHECK(ani, ANI_BASE);
GS_CHECK(sc, SC_BASE);
GS_CHECK(snd, SND_BASE);
GS_CHECK(cursor_bits, CBITS_BASE);
_Static_assert(sizeof(struct game_state) == DATA_END - DATA_BASE, "game_state covers 0x431000-0x4c0000");
extern struct game_state G;

/* After the asset is loaded (asset.h): G takes the data area of its image, and the image is
 * released. 1 ok. */
int state_init(void);

/* Field access with the width and zero extension of rd8/rd16/wr8/wr16 */
#define SV(f) ((uint32_t)G.save.n.f)
#define SV_SET(f, v) (G.save.n.f = (__typeof__(G.save.n.f))(v))
#define AN(f) ((uint32_t)G.an.f)
#define AN_SET(f, v) (G.an.f = (__typeof__(G.an.f))(v))
#define SC(f) ((uint32_t)G.sc.f)
#define SC_SET(f, v) (G.sc.f = (__typeof__(G.sc.f))(v))
#define MV(f) ((uint32_t)G.map.v.f)
#define MV_SET(f, val) (G.map.v.f = (__typeof__(G.map.v.f))(val))
#define RT(f) ((uint32_t)G.rt.f)
#define RT_SET(f, v) (G.rt.f = (__typeof__(G.rt.f))(v))

#endif
