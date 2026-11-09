#include "x86.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cpu cpu;
uint8_t *x86_mem;
int x86_poll_countdown = 1;
uint32_t x86_poll_pc;
uint64_t x86_icount;
void (*x86_trap_hook)(void);
uint32_t x86_stop_esp, x86_stop_pc;
const uint32_t *x86_stop_exits, *x86_stop_units, *x86_stop_calls;
int x86_stop_started;

/* ---- NANPA2_IMAGE_TRACE=FILE: which bytes of the PE image the run touched */
uint8_t *x86_img_map;
uint8_t x86_img_ctx = 1;
static const char *img_trace_file;

void x86_img_mark(uint32_t a, uint32_t n)
{
	uint32_t o = a - MEM_BASE;
	if (o + n > IMG_SIZE)
		n = IMG_SIZE - o;
	for (uint32_t i = 0; i < n; i++)
		x86_img_map[o + i] |= x86_img_ctx;
}

static void img_trace_dump(void)
{
	FILE *f = fopen(img_trace_file, "w");
	if (!f)
		return;
	/* one line per run of equal flags: start end(exclusive) flags */
	uint32_t i = 0;
	while (i < IMG_SIZE) {
		uint8_t v = x86_img_map[i];
		uint32_t j = i;
		while (j < IMG_SIZE && x86_img_map[j] == v)
			j++;
		if (v)
			fprintf(f, "%08x %08x %d\n", MEM_BASE + i, MEM_BASE + j, v);
		i = j;
	}
	fclose(f);
}

static void img_trace_start(void)
{
	img_trace_file = getenv("NANPA2_IMAGE_TRACE");
	if (!img_trace_file || !*img_trace_file)
		return;
	x86_img_map = calloc(1, IMG_SIZE);
	atexit(img_trace_dump);
}

int x86_stop_at(uint32_t pc)
{
	uint32_t esp = cpu.r[ESP];
	/* never at the first check: that is the oracle's own entry, which may
	 * be an exit (the loop head 0x4060f9) or lie above the stop ESP (the
	 * VM entry 0x4060d4 stops a word below, after its push ebp) */
	if (!x86_stop_started) {
		x86_stop_started = 1;
		return 0;
	}
	int stop = esp > x86_stop_esp;
	if (esp == x86_stop_esp) {
		for (const uint32_t *e = x86_stop_exits; e && *e; e++)
			stop |= *e == pc;
		/* another jump unit (or this one again) */
		for (const uint32_t *e = x86_stop_units; e && *e; e++)
			stop |= *e == pc;
	} else if (esp < x86_stop_esp && x86_stop_calls) {
		/* a call exit: the callee's entry with the return address on top
		 * (read past the memory trace) */
		uint32_t ra;
		memcpy(&ra, x86_ptr(esp, 4), 4);
		for (const uint32_t *c = x86_stop_calls; *c; c += 2)
			stop |= c[0] == pc && c[1] == ra;
	}
	if (stop) {
		x86_stop_pc = pc;
		x86_stop_esp = 0;
		/* lockstep: the shadow of the last block predicted this pc,
		 * not the sentinel x86_exec reports on return */
		X86_LOCKSTEP_POINT(LS_RET, pc);
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

int x86_exe_check(const uint8_t *d, size_t n)
{
	if (n < 0x400 || le32(d + 0x3c) > n - 0x100)
		return 0;
	uint32_t pe = le32(d + 0x3c);
	/* the Taiwan release built 2000-07-10 (PE TimeDateStamp 0x39697c6a,
	 * SizeOfImage 0xc1000): the native addresses throughout the engine
	 * are this build's */
	return le32(d + pe) == 0x4550u && le32(d + pe + 52) == MEM_BASE &&
	       le32(d + pe + 8) == 0x39697c6au && le32(d + pe + 80) == 0xc1000u;
}

/* The exe's sections laid out as the loader would, in a fresh zeroed image
 * of MEM_END - MEM_BASE bytes. NULL with *status 0: cannot read; -1: not the
 * nanpa2.exe this engine runs. *hash is the FNV-1a of the file. */
uint8_t *x86_image_read(const char *exe_path, int *status, uint64_t *hash, uint32_t *exe_size)
{
	*status = 0;
	FILE *f = fopen(exe_path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
	if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(d);
		return NULL;
	}
	fclose(f);
	if (!x86_exe_check(d, (size_t)n)) {
		free(d);
		*status = -1;
		return NULL;
	}
	uint8_t *mem = calloc(1, MEM_END - MEM_BASE);
	if (!mem) {
		free(d);
		return NULL;
	}
	uint32_t pe = le32(d + 0x3c);
	uint32_t nsec = le16(d + pe + 6);
	uint32_t hdr = le32(d + pe + 84);
	uint32_t secs = pe + 24 + le16(d + pe + 20);
	memcpy(mem, d, hdr < (uint32_t)n ? hdr : (uint32_t)n);
	for (uint32_t i = 0; i < nsec; i++) {
		const uint8_t *s = d + secs + i * 40;
		uint32_t va = le32(s + 12), rs = le32(s + 16), ro = le32(s + 20);
		if (ro + rs > (uint32_t)n)
			rs = (uint32_t)n - ro;
		memcpy(mem + va, d + ro, rs);
	}
	if (hash)
		*hash = x86_fnv64(d, (size_t)n, X86_FNV64_INIT);
	if (exe_size)
		*exe_size = (uint32_t)n;
	free(d);
	*status = 1;
	return mem;
}

uint64_t x86_fnv64(const void *p, size_t n, uint64_t h)
{
	const uint8_t *b = p;
	for (size_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 0x100000001b3ull;
	return h;
}

/* Memory is the given image (taken over); ready for the first call. */
void x86_install_image(uint8_t *mem)
{
	x86_mem = mem;
	cpu.r[ESP] = STACK_TOP - 16;
	img_trace_start();
}

int x86_code_absent;

int x86_load_image(const char *exe_path)
{
	int st;
	uint8_t *mem = x86_image_read(exe_path, &st, NULL, NULL);
	if (!mem)
		return st;
	x86_install_image(mem);
	return 1;
}

uint32_t x86_call(uint32_t addr, int nargs, const uint32_t *args)
{
	X86_LOCKSTEP_POINT(LS_NEST, addr);
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
