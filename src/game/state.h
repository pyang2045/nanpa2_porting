/*
 * The game state as C structs (docs/refactor.md, R1).
 *
 * The state used to live in the emulated 32-bit memory at the addresses of
 * nanpa2.exe's data and bss, read and written with rd16(0x4452e8) and the
 * like. A group of it that has moved is a field of the global G here, and
 * its address range is an alias (x86_alias, state_init): code that still
 * reads the address, the HLE layer, bulk copies and the script VM (whose
 * segments are addresses) get the object's bytes. The comments give the
 * native address of each field. New code names the field.
 *
 * Moved so far: the save image (struct save_image), the VM runtime block
 * (struct vm_rt), the screen, palette, cursor and window globals (struct
 * screen_state), the animation slots (struct anim_state).
 */
#ifndef NANPA2_GAME_STATE_H
#define NANPA2_GAME_STATE_H

#include <stddef.h>
#include <stdint.h>

#include "x86/x86.h"

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
	uint32_t top_esp;          /* 0x445cdc ESP of the top-level VM (after its push ebp) */
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
	uint32_t surface[4];       /* 0x44629e DirectDraw surface objects, 0 the screen */
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
 * timer callback (0x4028c0, src/hle/hle.c) steps the fade and blinks the cursor
 * from here. */
#define SC_BASE 0x4aac50u
#pragma pack(push, 1)
struct screen_state {
	uint32_t cursor_saved_y;   /* 0x4aac50 where the cursor sprite's background was saved */
	uint32_t cursor_saved_x;   /* 0x4aac54 */
	uint8_t pal_start[1024];   /* 0x4aac58 the palette entries as the fade started (RGBA x 256) */
	uint32_t scratch_surface;  /* 0x4ab058 the 640x480 scratch surface object (text) */
	uint32_t hwnd;             /* 0x4ab05c */
	int32_t pal_delta[256][3]; /* 0x4ab060 target - displayed, per channel */
	uint8_t cursor_saved[1024]; /* 0x4abc60 background under the cursor, 32 rows of 32 bytes */
	uint32_t cursor_y;         /* 0x4ac060 */
	uint32_t cursor_x;         /* 0x4ac064 */
	uint32_t letterbox;        /* 0x4ac068 1: the 640x480 image sits 40 lines down */
	uint32_t hinstance;        /* 0x4ac06c */
	uint32_t font[3];          /* 0x4ac070 font handles by size */
	uint8_t pal_entries[1024]; /* 0x4ac07c PALETTEENTRY[256]: red, green, blue, flags */
	uint8_t pad_47c[4];        /* 0x4ac47c */
	uint8_t surface_desc[0x24]; /* 0x4ac480 DDSURFACEDESC of the last Lock ... */
	uint32_t lock_ptr;         /* 0x4ac4a4 ... its lpSurface */
	uint8_t surface_desc_rest[0x44]; /* 0x4ac4a8 */
	uint32_t active;           /* 0x4ac4ec the timer runs the game only while set (WM_ACTIVATEAPP) */
	uint32_t ddraw;            /* 0x4ac4f0 */
	uint32_t palette_obj;      /* 0x4ac4f4 the DirectDraw palette object */
	uint32_t logo;             /* 0x4ac4f8 the logo movie is playing */
	uint32_t pad_4fc;          /* 0x4ac4fc */
	uint32_t timer_id;         /* 0x4ac500 */
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
_Static_assert(offsetof(struct anim_state, last_frame) == 0x4313bcu - AN_BASE, "last_frame");
_Static_assert(offsetof(struct anim_state, random_seed) == 0x4313c4u - AN_BASE, "random_seed");

struct game_state {
	struct save_image save;
	struct vm_rt rt;
	struct screen_state sc;
	struct anim_state an;
};
extern struct game_state G;

/* After the image is installed: the groups that have moved become aliases
 * of their addresses (taking the content the asset gave them). */
void state_init(void);

/* Field access with the width and zero extension of rd8/rd16/wr8/wr16 */
#define SV(f) ((uint32_t)G.save.n.f)
#define SV_SET(f, v) (G.save.n.f = (__typeof__(G.save.n.f))(v))
#define AN(f) ((uint32_t)G.an.f)
#define AN_SET(f, v) (G.an.f = (__typeof__(G.an.f))(v))
#define SC(f) ((uint32_t)G.sc.f)
#define SC_SET(f, v) (G.sc.f = (__typeof__(G.sc.f))(v))
#define RT(f) ((uint32_t)G.rt.f)
#define RT_SET(f, v) (G.rt.f = (__typeof__(G.rt.f))(v))

#endif
