/*
 * Script opcode handlers (R2: direct style).
 *
 * The VM loop (src/game/loop.c) fetches an opcode and calls its handler,
 * entered with vm_pc past the opcode bytes. A handler returns -1 (go on with
 * the loop: the native `jmp 0x4060f9`) or a block result (the native `ret`
 * out of the running block, AL: 1 continue, 2 break). Each backward jump of
 * the native handler polls the host (POLL), as the original code did.
 *
 * Semantics: re-notes.md "Script opcodes".
 */
#include "game/ctl.h"
#include "game/expr.h"
#include "game/vm.h"
#include "hle/hle.h"
#include "x86/x86.h"
#include "game/state.h"

/* back to the loop, as the native `jmp 0x4060f9` at native_pc */
static int to_loop(uint32_t pc, uint32_t native_pc)
{
	vm_pc = pc;
	POLL(native_pc);
	return -1;
}

/* There is another element after a 02 separator (polling for the native
 * loop's backward jump at native_pc); else the statement ends. */
static int another(uint32_t *pc, uint32_t native_pc)
{
	if (vm_script_byte(*pc) != VM_SEPARATOR)
		return 0;
	++*pc;
	POLL(native_pc);
	return 1;
}

/* flag n, n+1, ... = expressions separated by 02 */
static int set_flags(uint32_t pc, uint32_t n, uint32_t exit_pc, uint32_t loop_pc)
{
	do {
		script_set_flag(n++, vm_eval(&pc));
		COST(14);
	} while (another(&pc, loop_pc));
	return to_loop(pc, exit_pc);
}

/* 0a: the first flag number is a constant operand (0x4039f7; any other
 * byte is the number itself, the dispatcher having left EAX = 0). */
int vm_op_set_flags(void)
{
	uint32_t pc = vm_pc, n;
	script_read_constant(&pc, &n);
	COST(10);
	return set_flags(pc, n, 0x4064ab, 0x4064b3);
}

/* 0b: the first flag number is an expression. */
int vm_op_set_flags_expr(void)
{
	uint32_t pc = vm_pc;
	uint32_t n = vm_eval(&pc);
	return set_flags(pc, n, 0x4064cf, 0x4064d7);
}

/* The register named by a script byte ('@' = 0x40 ...), as an index: the
 * native code subtracts 0x40 in BL, so any byte names one of 256 words
 * from VM_REGS. */
static uint32_t reg_index(uint32_t *pc)
{
	return (vm_script_byte((*pc)++) - 0x40) & 0xff;
}

/* 0c R expr: register R = expr (16 bits). */
int vm_op_set_reg(void)
{
	uint32_t pc = vm_pc;
	uint32_t r = reg_index(&pc);
	vm_set_reg(r, vm_eval(&pc));
	COST(10);
	return to_loop(pc, 0x4064f3);
}

/* 0d R i v0 [02 v1 ...]: words at R + 2i, R + 2i + 2, ... of a segment
 * become v0, v1, ...: segment 0 when R is '@' (the word array is in the
 * script image), else the one @[7] selects. The choice is kept in
 * VM_STORE_SEGMENT and read back for each word. */
int vm_op_store_words(void)
{
	uint32_t pc = vm_pc;
	uint32_t r = reg_index(&pc);
	RT_SET(store_segment, vm_segment_select());
	if (r == 0)
		RT_SET(store_segment, 0);
	uint32_t base = vm_reg(r);
	uint32_t off = base + 2 * vm_eval(&pc);
	COST(16);
	do {
		uint32_t v = vm_eval(&pc);
		wr16(vm_segment_base(RT(store_segment)) + off, v);
		off += 2;
		COST(12);
	} while (another(&pc, 0x406555));
	return to_loop(pc, 0x40654b);
}

/* 0e R i v0 [02 v1 ...]: bytes at R + i, ... of the segment @[7] selects
 * (no '@' case here). */
int vm_op_store_bytes(void)
{
	uint32_t pc = vm_pc;
	uint32_t r = reg_index(&pc);
	uint32_t base = vm_reg(r);
	uint32_t off = base + vm_eval(&pc);
	COST(12);
	do {
		uint32_t v = vm_eval(&pc);
		wr8(vm_segment_base(vm_segment_select()) + off, v);
		off++;
		COST(14);
	} while (another(&pc, 0x40659c));
	return to_loop(pc, 0x406594);
}

/* 04 11 / 04 12: end the running block (a nested VM) with AL = 1
 * (continue) or 2 (break), which the enclosing while (04 10) reads. The
 * native `ret` at the top level has nothing to return to. */
static int end_block(int al)
{
	COST(2);
	if (vm_depth == 0)
		x86_trap(0x406638, "continue or break outside a block");
	return al;
}

int vm_op_continue(void)
{
	return end_block(1);
}

int vm_op_break(void)
{
	return end_block(2);
}

/* 12 n, offset: label n = offset (op 13 n calls it). */
int vm_op_set_label(void)
{
	uint32_t pc = vm_pc, n;
	vm_parse_args(&pc, &n);
	wr16(VM_LABELS + 2 * n, vm_arg(1));
	COST(6);
	return to_loop(pc, 0x406653);
}

/* 18 c: text colour = c. */
int vm_op_text_colour(void)
{
	uint32_t pc = vm_pc, c;
	vm_parse_args(&pc, &c);
	SV_SET(text_colour, c);
	COST(4);
	return to_loop(pc, 0x4069c9);
}

/* 04 20 button, offset: the block at offset runs on a click of the button
 * (0 left, 1 right), before each opcode fetch (0x40610b). */
int vm_op_click_handler(void)
{
	uint32_t pc = vm_pc, button;
	vm_parse_args(&pc, &button);
	wr16(VM_CLICK_HANDLERS + 2 * button, vm_arg(1));
	COST(3);
	return to_loop(pc, 0x4070d0);
}

/* 04 23 table, x, y: S = the id of the first rectangle in the table that
 * holds (x, y), or 0xffff. The table is at offset `table` of the segment
 * @[7] selects: records of five u16 (id, x0, y0, x1, y1), inclusive, ended
 * by id 0xffff. The segment base is read before the arguments. */
int vm_op_hit_test(void)
{
	uint32_t base = vm_segment_base(vm_segment_select());
	uint32_t pc = vm_pc, table;
	vm_parse_args(&pc, &table);
	uint32_t rec = base + table, x = vm_arg(1), y = vm_arg(2), id;
	COST(10);
	/* the timing probe of the native 0x4071bb: the exploration bot and
	 * NANPA2_HOTSPOTS read the rectangle table through it */
	probe_hit_table(rec);
	for (;; rec += 10) {
		id = rd16(rec);
		COST(8);
		if (id == 0xffff)
			break;
		/* the native order of the tests, so the same words are read */
		if (x >= rd16(rec + 2) && x <= rd16(rec + 6) && y >= rd16(rec + 4) && y <= rd16(rec + 8))
			break;
		POLL(0x4071f3);
	}
	vm_set_reg(VM_REG_S, id);
	return to_loop(pc, 0x4071ff);
}

/* 04 22 ...: read an argument list and do nothing with it. */
int vm_op_args_only(void)
{
	uint32_t pc = vm_pc, a;
	vm_parse_args(&pc, &a);
	return to_loop(pc, 0x4073a0);
}
