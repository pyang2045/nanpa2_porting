/*
 * Script opcode handlers (jump units, docs/reimpl.md "Jump overrides").
 *
 * The VM loop 0x4060f9 fetches an opcode and jumps to its handler through
 * the table at 0x40620b (ops 0a-1a) or, for 04 xx, 0x406262. A handler is
 * entered with ESI past the opcode bytes and leaves by jumping back to the
 * loop (or by `ret` out of a nested block). Here each handler is a
 * function that does the work on emulated memory, sets ESI and returns
 * the pc to go on at.
 *
 * At the loop head only ESP, EBP and ESI are live (tools/liveness.py), so
 * EAX, ECX, EDX, EBX, EDI and the flags are scratch for every handler that
 * goes back to the loop. Each backward jump of the native handler polls the
 * host (X86_POLL), as the lifted code does.
 *
 * Semantics: re-notes.md "Script opcodes".
 */
#include "game/expr.h"
#include "game/vm.h"
#include "x86/x86.h"

/* back to the loop, as the native `jmp 0x4060f9` at native_pc */
static uint32_t to_loop(uint32_t pc, uint32_t native_pc)
{
	cpu.r[ESI] = pc;
	X86_POLL(native_pc);
	return VM_LOOP;
}

/* There is another element after a 02 separator (polling for the native
 * loop's backward jump at native_pc); else the statement ends. */
static int another(uint32_t *pc, uint32_t native_pc)
{
	if (vm_script_byte(*pc) != VM_SEPARATOR)
		return 0;
	++*pc;
	X86_POLL(native_pc);
	return 1;
}

/* flag n, n+1, ... = expressions separated by 02 */
static uint32_t set_flags(uint32_t pc, uint32_t n, uint32_t exit_pc, uint32_t loop_pc)
{
	do {
		script_set_flag(n++, vm_eval(&pc));
		X86_COUNT(14);
	} while (another(&pc, loop_pc));
	return to_loop(pc, exit_pc);
}

/* 0a: the first flag number is a constant operand (0x4039f7; any other
 * byte is the number itself, the dispatcher having left EAX = 0). */
uint32_t game_op_set_flags(void)
{
	uint32_t pc = cpu.r[ESI], n;
	script_read_constant(&pc, &n);
	X86_COUNT(10);
	return set_flags(pc, n, 0x4064ab, 0x4064b3);
}

/* 0b: the first flag number is an expression. */
uint32_t game_op_set_flags_expr(void)
{
	uint32_t pc = cpu.r[ESI];
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
uint32_t game_op_set_reg(void)
{
	uint32_t pc = cpu.r[ESI];
	uint32_t r = reg_index(&pc);
	vm_set_reg(r, vm_eval(&pc));
	X86_COUNT(10);
	return to_loop(pc, 0x4064f3);
}

/* 0d R i v0 [02 v1 ...]: words at R + 2i, R + 2i + 2, ... of a segment
 * become v0, v1, ...: segment 0 when R is '@' (the word array is in the
 * script image), else the one @[7] selects. The choice is kept in
 * VM_STORE_SEGMENT and read back for each word. */
uint32_t game_op_store_words(void)
{
	uint32_t pc = cpu.r[ESI];
	uint32_t r = reg_index(&pc);
	wr32(VM_STORE_SEGMENT, vm_segment_select());
	if (r == 0)
		wr32(VM_STORE_SEGMENT, 0);
	uint32_t base = vm_reg(r);
	uint32_t off = base + 2 * vm_eval(&pc);
	X86_COUNT(16);
	do {
		uint32_t v = vm_eval(&pc);
		wr16(vm_segment_base(rd32(VM_STORE_SEGMENT)) + off, v);
		off += 2;
		X86_COUNT(12);
	} while (another(&pc, 0x406555));
	return to_loop(pc, 0x40654b);
}

/* 0e R i v0 [02 v1 ...]: bytes at R + i, ... of the segment @[7] selects
 * (no '@' case here). */
uint32_t game_op_store_bytes(void)
{
	uint32_t pc = cpu.r[ESI];
	uint32_t r = reg_index(&pc);
	uint32_t base = vm_reg(r);
	uint32_t off = base + vm_eval(&pc);
	X86_COUNT(12);
	do {
		uint32_t v = vm_eval(&pc);
		wr8(vm_segment_base(vm_segment_select()) + off, v);
		off++;
		X86_COUNT(14);
	} while (another(&pc, 0x40659c));
	return to_loop(pc, 0x406594);
}

/* 04 11 / 04 12: end the running block (a nested VM) with AL = 1
 * (continue) or 2 (break), which the enclosing while (04 10) reads. */
static uint32_t end_block(uint32_t al)
{
	set8l(EAX, al);
	X86_COUNT(2);
	return pop32();
}

uint32_t game_op_continue(void)
{
	return end_block(1);
}

uint32_t game_op_break(void)
{
	return end_block(2);
}

/* 12 n, offset: label n = offset (op 13 n calls it). */
uint32_t game_op_set_label(void)
{
	uint32_t pc = cpu.r[ESI], n;
	vm_parse_args(&pc, &n);
	wr16(VM_LABELS + 2 * n, vm_arg(1));
	X86_COUNT(6);
	return to_loop(pc, 0x406653);
}

/* 18 c: text colour = c. */
uint32_t game_op_text_colour(void)
{
	uint32_t pc = cpu.r[ESI], c;
	vm_parse_args(&pc, &c);
	wr16(VM_TEXT_COLOUR, c);
	X86_COUNT(4);
	return to_loop(pc, 0x4069c9);
}

/* 04 20 button, offset: the block at offset runs on a click of the button
 * (0 left, 1 right), before each opcode fetch (0x40610b). */
uint32_t game_op_click_handler(void)
{
	uint32_t pc = cpu.r[ESI], button;
	vm_parse_args(&pc, &button);
	wr16(VM_CLICK_HANDLERS + 2 * button, vm_arg(1));
	X86_COUNT(3);
	return to_loop(pc, 0x4070d0);
}

/* 04 23 table, x, y: S = the id of the first rectangle in the table that
 * holds (x, y), or 0xffff. The table is at offset `table` of the segment
 * @[7] selects: records of five u16 (id, x0, y0, x1, y1), inclusive, ended
 * by id 0xffff. The segment base is read before the arguments. */
uint32_t game_op_hit_test(void)
{
	uint32_t base = vm_segment_base(vm_segment_select());
	uint32_t pc = cpu.r[ESI], table;
	vm_parse_args(&pc, &table);
	uint32_t rec = base + table, x = vm_arg(1), y = vm_arg(2), id;
	X86_COUNT(10);
	/* the timing probe of the native 0x4071bb (overrides.h), with edi = the
	 * rectangle table as there: the exploration bot and NANPA2_HOTSPOTS
	 * read the table through it */
	cpu.r[EDI] = rec;
	x86_probe(0x4071bb);
	for (;; rec += 10) {
		id = rd16(rec);
		X86_COUNT(8);
		if (id == 0xffff)
			break;
		/* the native order of the tests, so the same words are read */
		if (x >= rd16(rec + 2) && x <= rd16(rec + 6) && y >= rd16(rec + 4) && y <= rd16(rec + 8))
			break;
		X86_POLL(0x4071f3);
	}
	vm_set_reg(VM_REG_S, id);
	return to_loop(pc, 0x4071ff);
}

/* 04 22 ...: read an argument list and do nothing with it. */
uint32_t game_op_args_only(void)
{
	uint32_t pc = cpu.r[ESI], a;
	vm_parse_args(&pc, &a);
	return to_loop(pc, 0x4073a0);
}
