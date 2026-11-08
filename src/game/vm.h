/*
 * Script VM state, read and written at its native addresses.
 *
 * The reimplemented game logic (src/game) keeps emulated memory as its only
 * state store, so lifted and reimplemented code see the same variables. This
 * header names those variables; see re-notes.md "Script VM state".
 */
#ifndef NANPA2_GAME_VM_H
#define NANPA2_GAME_VM_H

#include <stdint.h>

#include "x86/x86.h"

/* MES script image (loaded by 0x401050 from the file named at
 * VM_SCRIPT_NAME). The VM's program counter (ESI in the native code) is an
 * offset from here, and segment 0 starts here. */
#define VM_SCRIPT 0x435e78u
#define VM_SCRIPT_NAME 0x444e78u

/* 4-bit script flags, two per byte: flag n is in byte n/2, the low nibble
 * for even n, the high nibble for odd n. */
#define VM_FLAGS 0x444e98u

/* Registers '@' (0x40) to 'Z' (0x5a): 27 u16. Register '@' holds the
 * segment-0 offset of the word array VM_ARRAY (set by 0x406395), so
 * expressions index the array through it. */
#define VM_REGS 0x445298u
#define VM_REG_COUNT 27
#define VM_ARRAY 0x4452ceu

/* Word 7 of that array (@[7]) selects the data segment that array
 * expressions and assignments use. */
#define VM_SEGMENT_SELECT 0x4452dcu

/* Segment base addresses (u32 each): 0 = the script image, 1 = 0x4462be,
 * 2 and 3 = 0x4562be (0x406395). */
#define VM_SEGMENTS 0x44533cu

/* Seed of the script random number generator (u16). */
#define VM_RANDOM_SEED 0x4313c4u

/* Set by 0x406395 next to '@': word 0 of the array (@[0]) holds the
 * segment-0 offset of a work buffer, word 22 (@[22]) that of the menu item
 * list that op 04 14 fills. */
#define VM_ARRAY0_BUFFER 0x445396u
#define VM_MENU_LIST_PTR 0x4452fau
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
#define VM_TEXT_COLOUR 0x4452f4u
/* Segment that op 0d stores into: u32, @[7] or 0 for register '@'. */
#define VM_STORE_SEGMENT 0x44629au

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

/* `call addr` into native game code, an import or an HLE routine, with the
 * CPU as it is: the callee runs on the emulated stack and leaves the CPU
 * as it does. The caller pushed the arguments; `popped` of them are the
 * callee's to pop (stdcall), the rest the caller's. Traps if the stack
 * does not come back to that. No callee used so may leave by the VM's
 * stack reset (0x40638d). */
void game_ncall(uint32_t addr, uint32_t popped);

/* Register-level entry points (src/game/units.c). */
void game_skip_block_regs(void);   /* 0x4061d4 */
void game_vm_init_regs(void);      /* 0x406395 */
void game_menu_item_regs(void);    /* 0x4068e1 */
void game_string_arg_regs(void);   /* 0x406dc4 */
void game_parse_args_regs(void);   /* 0x40741b */

/* ---- src/game/ops.c: opcode handlers (jump units) ---- */
uint32_t game_op_set_flags(void);       /* 0a 0x406491 */
uint32_t game_op_set_flags_expr(void);  /* 0b 0x4064b5 */
uint32_t game_op_set_reg(void);         /* 0c 0x4064d9 */
uint32_t game_op_store_words(void);     /* 0d 0x4064f8 */
uint32_t game_op_store_bytes(void);     /* 0e 0x406557 */
uint32_t game_op_continue(void);        /* 04 11 0x406638 */
uint32_t game_op_break(void);           /* 04 12 0x40663b */
uint32_t game_op_set_label(void);       /* 12 0x40663e */
uint32_t game_op_text_colour(void);     /* 18 0x4069be */
uint32_t game_op_click_handler(void);   /* 04 20 0x4070bc */
uint32_t game_op_hit_test(void);        /* 04 23 0x4071a4 */
uint32_t game_op_args_only(void);       /* 04 22 0x40739b */

/* ---- src/game/ops_vm.c: handlers split at the blocks and waits they run ---- */
uint32_t game_op_if(void);               /* 0f 0x40659e */
uint32_t game_op_if_then(void);          /* 0f 0x4065ae */
uint32_t game_op_if_else(void);          /* 0f 0x4065ed */
uint32_t game_op_wait(void);             /* 11 0x4065f6 */
uint32_t game_op_wait_clear(void);       /* 11 0x4065fb */
uint32_t game_op_while(void);            /* 04 10 0x40660c */
uint32_t game_op_while_next(void);       /* 04 10 0x40661e */
uint32_t game_op_call_label(void);       /* 13 0x406658 */
uint32_t game_op_call_label_done(void);  /* 13 0x40666b */
uint32_t game_op_call_offset(void);      /* 14 0x406a9a */
uint32_t game_op_call_offset_done(void); /* 14 0x406aa7 */
uint32_t game_op_delay(void);            /* 16 0x406aad */
uint32_t game_op_delay_done(void);       /* 16 0x406abd */
uint32_t game_op_delay_click_done(void); /* 16 0x406ac7 */
uint32_t game_op_print_number(void);      /* 15 0x4069ce */
uint32_t game_op_print_number_done(void); /* 15 0x4069f7 */
void game_format_number_regs(void);       /* 0x4069fd */
uint32_t game_op_jump_mes(void);          /* 04 1e 0x406f7c */
uint32_t game_op_call_mes(void);          /* 04 1f 0x406fcd */
uint32_t game_op_call_mes_done(void);     /* 04 1f 0x40704a */
uint32_t game_op_flag_file(void);         /* 04 21 0x4070d5 */
uint32_t game_op_menu(void);              /* 04 13 0x406671 */
uint32_t game_op_menu_round(void);        /* 04 13 0x4066b8 */
uint32_t game_op_menu_pumped(void);       /* 04 13 0x4066bd */
uint32_t game_op_menu_framed(void);       /* 04 13 0x4066e6 */
uint32_t game_op_menu_tested0(void);  /* 04 13 0x4066f6 */
uint32_t game_op_menu_handled0(void); /* 04 13 0x406704 */
uint32_t game_op_menu_waited0(void);  /* 04 13 0x406714 */
uint32_t game_op_menu_tested1(void);  /* 04 13 0x406728 */
uint32_t game_op_menu_handled1(void); /* 04 13 0x406736 */
uint32_t game_op_menu_waited1(void);  /* 04 13 0x40674a */
uint32_t game_op_menu_tested2(void);  /* 04 13 0x406761 */
uint32_t game_op_menu_handled2(void); /* 04 13 0x40676f */
uint32_t game_op_menu_waited2(void);  /* 04 13 0x406783 */
uint32_t game_op_menu_tested3(void);  /* 04 13 0x40679a */
uint32_t game_op_menu_handled3(void); /* 04 13 0x4067a8 */
uint32_t game_op_menu_waited3(void);  /* 04 13 0x4067bc */
uint32_t game_op_menu_tested4(void);  /* 04 13 0x4067d3 */
uint32_t game_op_menu_handled4(void); /* 04 13 0x4067e1 */
uint32_t game_op_menu_waited4(void);  /* 04 13 0x4067f5 */
uint32_t game_op_menu_tested5(void);  /* 04 13 0x406810 */
uint32_t game_op_menu_handled5(void); /* 04 13 0x406822 */
uint32_t game_op_menu_waited5(void);  /* 04 13 0x40683b */

/* ---- src/game/ops_sys.c: handlers around native subsystem routines ---- */
uint32_t game_op_menu_items(void);      /* 04 14 0x406842 */
uint32_t game_op_cursor(void);          /* 04 15 0x406916 */
uint32_t game_op_clear_text(void);      /* 17 0x406b05 */
uint32_t game_op_fill_rect(void);       /* 04 17 0x406b76 */
uint32_t game_op_xor_rect(void);        /* 04 18 0x406bcb */
uint32_t game_op_copy_rect(void);       /* 04 19 0x406c20 */
uint32_t game_op_swap_rect(void);       /* 04 1a 0x406c85 */
uint32_t game_op_masked_rect(void);     /* 04 1b 0x406cea */
uint32_t game_op_load_file(void);       /* 04 1c 0x406d8b */
uint32_t game_op_picture(void);         /* 04 1d 0x406e11 */
uint32_t game_op_palette(void);         /* 04 16 0x406f06 */
uint32_t game_op_anim(void);            /* 1a 0x407096 */
uint32_t game_op_anim_done(void);       /* 1a 0x4070b6 */
uint32_t game_op_misc(void);            /* 19 0x407204 */
uint32_t game_op_music(void);           /* 04 24 0x4073a5 */
uint32_t game_op_voice(void);           /* 04 25 0x4073cd */
uint32_t game_op_map(void);             /* 04 26 0x4073ee */
/* the cases each leaves to the native handler (struct game_unit.native) */
int game_native_misc(void);
int game_native_palette(void);
int game_native_map(void);

/* ---- src/game/loop.c: the VM loop, blocks, text, waits (stage 4) ---- */
uint32_t game_vm_entry(void);           /* 0x4060d4 */
uint32_t game_vm_loop(void);            /* 0x4060f9 */
uint32_t game_vm_pumped(void);          /* 0x4060fe */
uint32_t game_vm_click_tested0(void);   /* 0x406124 */
uint32_t game_vm_click_tested1(void);   /* 0x406134 */
uint32_t game_vm_block_done(void);      /* 0x4061c2 */
uint32_t game_vm_run_block(void);       /* 0x4061cd */
uint32_t game_vm_block_return(void);    /* 0x4061d3 */
uint32_t game_vm_string_next(void);     /* 0x406315 */
uint32_t game_vm_big5_next(void);       /* 0x406383 */
uint32_t game_input_test(void);         /* 0x4074d5 */
uint32_t game_input_tested(void);       /* 0x4074db */
uint32_t game_wait_frames(void);        /* 0x4075c6 */
uint32_t game_wait_frames_pumped(void); /* 0x4075d7 */
uint32_t game_wait_click(void);         /* 0x406acc */
uint32_t game_wait_click_framed(void);  /* 0x406adf */
uint32_t game_wait_click_tested(void);  /* 0x406ae6 */
uint32_t game_wait_click_release_framed(void); /* 0x406afb */
uint32_t game_wait_click_release_tested(void); /* 0x406b02 */
int game_native_click0(void);
int game_native_click1(void);

/* ---- src/game/text.c ---- */
void game_draw_char(uint32_t c, uint32_t x, uint32_t y, uint32_t size, uint32_t fg,
                    uint32_t bg);       /* 0x401900 */
void game_draw_char_regs(void);
void game_text_advance(void);           /* 0x4063ee */
void game_text_newline(void);           /* 0x406415 */
void game_clear_text_window(void);      /* 0x406b24, EBX = colour */

/* ---- src/game/anim.c ---- */
void game_anim_step(void);              /* 0x403624 */
void game_anim_run_slot(void);          /* 0x403661, ESI = slot offset */
void game_anim_control_regs(void);      /* 0x403510, EAX sub-op, EBX slot */
uint32_t game_anim_control_piece(void); /* 0x403510 as a jump unit (any sub-op) */
uint32_t game_anim_wait_piece(void);    /* 0x4035b5: sub-op 4's wait loop */
void game_anim_reset(void);             /* 0x406dff */
void game_palette_copy(uint32_t src);   /* 0x40752c */
void game_palette_copy_regs(void);

static inline uint32_t vm_script_byte(uint32_t pc)
{
	return rd8(VM_SCRIPT + pc);
}

static inline uint32_t vm_reg(uint32_t r)
{
	return rd16(VM_REGS + 2u * r);
}

static inline void vm_set_reg(uint32_t r, uint32_t v)
{
	wr16(VM_REGS + 2u * r, v);
}

static inline uint32_t vm_segment_base(uint32_t seg)
{
	return rd32(VM_SEGMENTS + 4u * seg);
}

static inline uint32_t vm_segment_select(void)
{
	return rd16(VM_SEGMENT_SELECT);
}

#endif
