/*
 * Script VM state, read and written at its native addresses.
 *
 * The state is in G (state.h); the script image, its segments and the VM's
 * address-valued words are still addressed natively through the alias. This
 * header names those addresses; see re-notes.md "Script VM state".
 */
#ifndef NANPA2_GAME_VM_H
#define NANPA2_GAME_VM_H

#include <stdint.h>

#include "game/state.h"
#include "x86/x86.h"

/* MES script image (loaded by 0x401050 from the file named at
 * VM_SCRIPT_NAME). The VM's program counter (ESI in the native code) is an
 * offset from here, and segment 0 starts here. */
#define VM_SCRIPT 0x435e78u
#define VM_SCRIPT_NAME 0x444e78u

/* The saved state (flags, registers, the word array @[i], the text window) is
 * G.save (state.h): flags are 4-bit, two per byte, flag n in byte n/2 (low
 * nibble for even n). Registers '@' (0x40) to 'Z' (0x5a): 27 u16. Register '@' holds the
 * segment-0 offset of the word array VM_ARRAY (set by 0x406395), so
 * expressions index the array through it. */
#define VM_REG_COUNT 27
#define VM_ARRAY 0x4452ceu

/* @[7] (G.save.n.segment_select) selects the data segment that array
 * expressions and assignments use. Segment base addresses (G.save.n.segment,
 * u32 each): 0 = the script image, 1 = 0x4462be, 2 and 3 = 0x4562be
 * (0x406395). */

/* Seed of the script random number generator (u16). */
#define VM_RANDOM_SEED 0x4313c4u

/* Set by 0x406395 next to '@': word 0 of the array (@[0]) holds the
 * segment-0 offset of a work buffer, word 22 (@[22]) that of the menu item
 * list that op 04 14 fills. */
#define VM_ARRAY0_BUFFER 0x445396u
#define VM_MENU_LIST 0x445d90u
/* Segment bases 1-3. */
#define VM_SEGMENT1 0x4462beu
#define VM_SEGMENT2 0x4562beu

/* Argument list of the last opcode (0x40741b): a type byte per argument
 * (VM_ARG_*; the parser clears the first 12) and a u16 value per argument
 * (an expression's value, or the script offset of a string or block). */
#define VM_ARG_TYPES 0x445cf6u
#define VM_ARG_VALUES 0x445d06u
#define VM_ARG_TYPES_CLEARED 12
enum { VM_ARG_EXPR = 1, VM_ARG_STRING = 2, VM_ARG_BLOCK = 3 };

/* Script offsets of labels (op 12 sets, op 13 calls): u16 each. */
#define VM_LABELS 0x445dbcu
/* Click handler labels (op 04 20): 0 left button, 1 right; u16 offsets. */
#define VM_CLICK_HANDLERS 0x445ce2u
/* Text colour (op 18): u16, low nibble foreground, next nibble background. */
/* Segment that op 0d stores into: u32, @[7] or 0 for register '@'. */

/* Register S ('S' = 0x53, index 0x13): results of ops 19, 04 23, 04 26 and
 * menus. */
#define VM_REG_S 0x13u

/* Script bytes with a meaning of their own. */
enum {
	VM_BLOCK_END = 0x00,   /* end of a block (and of a menu list) */
	VM_BLOCK_OPEN = 0x01,  /* start of a block */
	VM_SEPARATOR = 0x02,   /* between list items and arguments; else-if */
	VM_STRING = 0x06,      /* opens and closes a string */
	VM_IF = 0x0f,          /* if (also a menu item's guard) */
};

/* The VM loop head 0x4060f9: where opcode handlers go on. */
#define VM_LOOP 0x4060f9u

static inline void vm_set_script_byte(uint32_t pc, uint32_t v)
{
	wr8(VM_SCRIPT + pc, v);
}

static inline uint32_t vm_arg(uint32_t i)
{
	return rd16(VM_ARG_VALUES + 2u * i);
}

/* ---- src/game/vm.c ---- */

/* Evaluate an expression (script_eval), charging the native evaluator's
 * time. A bare token where a value has to be is a script error: trap. */
uint32_t vm_eval(uint32_t *pc);
/* 0x4061d4: from pc, find the next 01 and return the offset just past its
 * matching 00. */
uint32_t vm_skip_block(uint32_t pc);
/* 0x40741b: parse an argument list at *pc into VM_ARG_TYPES/VALUES and
 * return the number of arguments. *eax gets the native EAX (the value of
 * argument 0, see re-notes.md). */
uint32_t vm_parse_args(uint32_t *pc, uint32_t *eax);



/* ---- the VM (R2: direct style; src/game/loop.c, ops.c, ops_vm.c) ---- */

/* the program counter (the native ESI): an offset in segment 0 */
extern uint32_t vm_pc;
/* nested vm_loop() calls above the top level (the native ESP != [top ESP]) */
extern int vm_depth;

/* 0x4060d4 after its set-up: load the MES file named at VM_SCRIPT_NAME and
 * run the VM until the script ends or the game quits */
void game_vm_run(void);
/* 0x4060f9: run until a block ends; the block result AL (0 end, 1 continue,
 * 2 break) */
int vm_loop(void);
/* 0x4061cd: run the block at vm_pc (its 01) as a nested loop; its AL. The
 * caller charges its own `call`. */
int vm_run_block(void);
/* `call 0x4060f9` from vm_pc: a nested loop (called scripts, digits) */
int vm_call_loop(void);
/* a restart of the top-level loop (04 1e, 04 21 from a nested VM) */
_Noreturn void vm_restart(void);
/* 0x4074d5: pump, then 1 if input k is down. 0x4075c6: wait n / 4 + 1 frames.
 * 0x406acc: wait for a click. They pump; WM_QUIT leaves by ctl_quit(). */
int vm_input_down(uint32_t k);
void vm_wait_frames(uint32_t n);
void vm_wait_click(void);
/* 0x406395: set up '@', the segments and the word array's pointers */
void vm_init(void);
/* 0x406dc4: argument 0 copied as a string to dst; the new dst; *src_end the
 * script offset past it, *last its last byte */
uint32_t vm_string_arg(uint32_t dst, uint32_t *src_end, uint32_t *last);

/* Opcode handlers: entered with vm_pc past the opcode bytes, they return -1
 * (on with the loop) or the block result (the native `ret` out of the running
 * block). The ops that are still register-level pieces are below. */
int vm_op_set_flags(void);       /* 0a */
int vm_op_set_flags_expr(void);  /* 0b */
int vm_op_set_reg(void);         /* 0c */
int vm_op_store_words(void);     /* 0d */
int vm_op_store_bytes(void);     /* 0e */
int vm_op_if(void);              /* 0f */
int vm_op_wait(void);            /* 11 */
int vm_op_set_label(void);       /* 12 */
int vm_op_call_label(void);      /* 13 */
int vm_op_call_offset(void);     /* 14 */
int vm_op_print_number(void);    /* 15 */
int vm_op_delay(void);           /* 16 */
int vm_op_text_colour(void);     /* 18 */
int vm_op_while(void);           /* 04 10 */
int vm_op_continue(void);        /* 04 11 */
int vm_op_break(void);           /* 04 12 */
int vm_op_menu(void);            /* 04 13 */
int vm_op_jump_mes(void);        /* 04 1e */
int vm_op_call_mes(void);        /* 04 1f */
int vm_op_click_handler(void);   /* 04 20 */
int vm_op_flag_file(void);       /* 04 21 */
int vm_op_args_only(void);       /* 04 22 */
int vm_op_clear_text(void);      /* 17 */
int vm_op_fill_rect(void);       /* 04 17 */
int vm_op_xor_rect(void);        /* 04 18 */
int vm_op_copy_rect(void);       /* 04 19 */
int vm_op_swap_rect(void);       /* 04 1a */
int vm_op_masked_rect(void);     /* 04 1b */
int vm_op_menu_items(void);      /* 04 14 */
int vm_op_cursor(void);          /* 04 15 */
int vm_op_palette(void);         /* 04 16 */
int vm_op_load_file(void);       /* 04 1c */
int vm_op_picture(void);         /* 04 1d */
int vm_op_music(void);           /* 04 24 */
int vm_op_voice(void);           /* 04 25 */
int vm_op_anim(void);            /* 1a */
int vm_op_misc(void);            /* 19 */
int vm_op_map(void);             /* 04 26 (map_pump.c) */
/* 0x4068e1: one menu list item at *pc: 0xff at the end, else the guard's low
 * byte; *text = the item's text offset, *pc the next 00 or 02 */
uint32_t vm_menu_item(uint32_t *pc, uint32_t *text);
int vm_op_hit_test(void);        /* 04 23 */

/* ---- src/game/screen.c (stage 5B): palette and fades ---- */
void game_palette_save(void);           /* 0x40756c */
void game_palette_restore(void);        /* 0x407599 */
void game_palette_fill(uint16_t v);     /* 0x40754d */
void game_palette_upload(void);         /* 0x401ca0 */
uint32_t game_fade_start(void);         /* 0x401d30; the last blue distance (EAX) */
uint32_t game_fade_active(void);        /* 0x401e10 */
uint32_t game_wipe(uint32_t s0, uint32_t s1); /* 0x401a50: the dissolve wipe of op 19 4 (surfaces 0, 2); pumps; EAX of the last unlock */
void game_palette_wait(void);           /* 0x406f41: 04 16 3 waits for the fade, restores the tables */

/* surfaces, cursor, rectangles (screen.c). Surface arguments of the lock and unlock are the
 * DirectDraw objects (guest addresses); the pixel pointer is a guest address (x86_ptr). */
uint32_t game_surface_lock(uint32_t surf);    /* 0x401190 */
uint32_t game_surface_unlock(uint32_t surf);  /* 0x4012b0; EAX as the routine leaves it */
uint32_t game_frame_alloc(uint32_t bytes); /* scratch on the emulated stack for the HLE boundary */
void game_frame_free(uint32_t bytes);
void game_cursor_restore(void);         /* 0x4011f0 */
uint32_t game_cursor_draw(void);        /* 0x4012e0; the cursor's x (EAX) */
void game_cursor_show(void);            /* 0x401c40 */
void game_cursor_hide(void);            /* 0x401c60 */
void game_cursor_setpos(uint32_t x, uint32_t y); /* 0x401c80 */
/* the rectangles: (dx, dy, sx, sy, w, h) as 32-bit words the handler computed, surfaces src, dst */
void game_blit_copy(uint32_t dx, uint32_t dy, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                    uint8_t src, uint8_t dst);   /* 0x401430 */
void game_blit_masked(uint32_t dx, uint32_t dy, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                      uint8_t src, uint8_t dst); /* 0x4014f0 */
void game_blit_swap(uint32_t dx, uint32_t dy, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                    uint8_t src, uint8_t dst);   /* 0x401670 */
void game_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour,
                    uint32_t surf);              /* 0x4017b0, colour << 4 */
void game_xor_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour,
                   uint32_t surf);               /* 0x401870 */
uint32_t game_picture_load(int named);  /* 0x406e20: named = load the file named by arg 0; EAX of the last unlock */

/* ---- src/game/text.c ---- */
void game_draw_char(uint16_t c, uint32_t x, uint32_t y, uint32_t size, uint8_t fg, uint8_t bg); /* 0x401900 */
void game_text_advance(void);           /* 0x4063ee */
void game_text_newline(void);           /* 0x406415 */
void game_clear_text_window(uint32_t colour); /* 0x406b24, colour << 4 (the fill's) */

/* ---- src/game/anim.c ---- */
void game_anim_step(void);              /* 0x403624 */
/* 0x403510 (and the wait loop 0x4035b5 of sub-op 4): sub-op, slot, operands */
void game_anim_control(uint32_t op, uint32_t n, uint32_t a, uint32_t b);
void game_anim_reset(void);             /* 0x406dff */
void game_palette_copy(const uint16_t *src); /* 0x40752c */
void game_palette_copy_at(uint32_t src);     /* the same, by guest address */

static inline uint32_t vm_script_byte(uint32_t pc)
{
	return rd8(VM_SCRIPT + pc);
}

static inline uint32_t vm_reg(uint32_t r)
{
	return G.save.reg[r];
}

static inline void vm_set_reg(uint32_t r, uint32_t v)
{
	G.save.reg[r] = (uint16_t)v;
}

static inline uint32_t vm_segment_base(uint32_t seg)
{
	return G.save.n.segment[seg];
}

static inline uint32_t vm_segment_select(void)
{
	return SV(segment_select);
}

#endif
