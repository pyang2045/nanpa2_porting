#include "x86.h"

#include <stdio.h>
#include <stdlib.h>

struct cpu cpu;
uint8_t *x86_mem;
int x86_poll_countdown = 1;
uint32_t x86_poll_pc;
uint64_t x86_icount;
void (*x86_trap_hook)(void);
uint32_t x86_stop_esp, x86_stop_pc;
const uint32_t *x86_stop_exits;

int x86_stop_at(uint32_t pc)
{
	uint32_t esp = cpu.r[ESP];
	int stop = esp > x86_stop_esp;
	if (esp == x86_stop_esp)
		for (const uint32_t *e = x86_stop_exits; e && *e; e++)
			stop |= *e == pc;
	if (stop) {
		x86_stop_pc = pc;
		x86_stop_esp = 0;
	}
	return stop;
}

_Noreturn void x86_trap(uint32_t pc, const char *why)
{
	fprintf(stderr, "x86 trap at %08x: %s\n", pc, why);
	fprintf(stderr, "  eax=%08x ecx=%08x edx=%08x ebx=%08x\n  esp=%08x ebp=%08x esi=%08x edi=%08x\n",
		cpu.r[EAX], cpu.r[ECX], cpu.r[EDX], cpu.r[EBX], cpu.r[ESP], cpu.r[EBP], cpu.r[ESI],
		cpu.r[EDI]);
	if (cpu.r[ESP] >= MEM_BASE && cpu.r[ESP] + 32 < MEM_END) {
		fprintf(stderr, "  stack:");
		for (int i = 0; i < 8; i++)
			fprintf(stderr, " %08x", rd32(cpu.r[ESP] + 4 * i));
		fprintf(stderr, "\n");
	}
	if (x86_trap_hook)
		x86_trap_hook();
	abort();
}

_Noreturn void x86_fault(uint32_t addr)
{
	fprintf(stderr, "x86 memory fault at %08x\n", addr);
	x86_trap(0, "memory fault");
}

void x86_divide_error(void)
{
	x86_trap(0, "divide error");
}

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t le16(const uint8_t *p)
{
	return p[0] | p[1] << 8;
}

int x86_load_image(const char *exe_path)
{
	FILE *f = fopen(exe_path, "rb");
	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc((size_t)n);
	if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(d);
		return 0;
	}
	fclose(f);

	x86_mem = calloc(1, MEM_END - MEM_BASE);
	if (!x86_mem)
		return 0;
	uint32_t pe = le32(d + 0x3c);
	uint32_t nsec = le16(d + pe + 6);
	uint32_t base = le32(d + pe + 52);
	uint32_t hdr = le32(d + pe + 84);
	uint32_t secs = pe + 24 + le16(d + pe + 20);
	if (base != MEM_BASE) {
		free(d);
		return 0;
	}
	memcpy(x86_mem, d, hdr < (uint32_t)n ? hdr : (uint32_t)n);
	for (uint32_t i = 0; i < nsec; i++) {
		const uint8_t *s = d + secs + i * 40;
		uint32_t va = le32(s + 12), rs = le32(s + 16), ro = le32(s + 20);
		if (ro + rs > (uint32_t)n)
			rs = (uint32_t)n - ro;
		memcpy(x86_mem + va, d + ro, rs);
	}
	free(d);
	cpu.r[ESP] = STACK_TOP - 16;
	return 1;
}

uint32_t x86_call(uint32_t addr, int nargs, const uint32_t *args)
{
	struct cpu saved = cpu;
	for (int i = nargs - 1; i >= 0; i--)
		push32(args[i]);
	push32(X86_RET_SENTINEL);
	x86_exec(addr);
	uint32_t ret = cpu.r[EAX];
	uint32_t esp = saved.r[ESP];
	cpu = saved;
	cpu.r[ESP] = esp;
	return ret;
}

/* ---------------------------------------------------------------- heap */

/* First-fit allocator over [HEAP_BASE, MEM_END). Each block has an 8-byte
 * header: size (including header) and a used flag. */
static uint32_t heap_top = HEAP_BASE;

uint32_t x86_malloc(uint32_t size)
{
	size = (size + 8 + 15) & ~15u;
	for (uint32_t b = HEAP_BASE; b < heap_top; b += rd32(b)) {
		uint32_t bs = rd32(b);
		if (rd32(b + 4) || bs < size)
			continue;
		if (bs - size >= 64) {
			wr32(b + size, bs - size);
			wr32(b + size + 4, 0);
			wr32(b, size);
		}
		wr32(b + 4, 1);
		memset(x86_ptr(b + 8, rd32(b) - 8), 0, rd32(b) - 8);
		return b + 8;
	}
	if (heap_top + size > MEM_END)
		return 0;
	uint32_t b = heap_top;
	heap_top += size;
	wr32(b, size);
	wr32(b + 4, 1);
	return b + 8;
}

void x86_free(uint32_t addr)
{
	if (!addr)
		return;
	uint32_t b = addr - 8;
	wr32(b + 4, 0);
	/* merge with following free blocks */
	for (;;) {
		uint32_t nb = b + rd32(b);
		if (nb >= heap_top || rd32(nb + 4))
			break;
		wr32(b, rd32(b) + rd32(nb));
	}
	if (b + rd32(b) == heap_top)
		heap_top = b;
}
