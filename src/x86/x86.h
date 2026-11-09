/*
 * The emulated machine that the C engine still sits on (docs/refactor.md): a
 * flat little-endian address space (the exe's data at its native addresses, the
 * emulated stack and heap) with the game state aliased onto the C object G, and
 * the eight registers the HLE layer reads its arguments from. No game code runs
 * on it any more (R2): what is left is the Win32/DirectDraw/CRT thunk layer
 * (src/hle), which R4 replaces, and the instruction counter that paces the
 * game (R3 replaces it).
 */
#ifndef NANPA2_X86_H
#define NANPA2_X86_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

struct cpu {
	uint32_t r[8];
};
extern struct cpu cpu;
extern uint8_t *x86_mem;

#define MEM_BASE 0x00400000u
#define MEM_END 0x04000000u
#define STACK_TOP 0x01000000u
#define HEAP_BASE 0x01000000u
/* Game code run by the backends; everything else goes to hle_call(). */
#define CODE_START 0x00401000u
#define CODE_END 0x00407800u
/* Return address the host pushes before entering lifted code. */
#define X86_RET_SENTINEL 0xfffffff0u
_Noreturn void x86_trap(uint32_t pc, const char *why);
/* called by x86_trap after the register dump (dev logging), may be NULL */
extern void (*x86_trap_hook)(void);
_Noreturn void x86_fault(uint32_t addr);

/* Dev trace of the PE image region (NANPA2_IMAGE_TRACE=1, src/x86/image.c):
 * one byte per image byte, OR-ed with x86_img_ctx (1 = data access, 2 = the
 * interpreter decoding code) for every byte x86_ptr hands out. */
#define IMG_SIZE 0xc1000u
extern uint8_t *x86_img_map;
extern uint8_t x86_img_ctx;
void x86_img_mark(uint32_t a, uint32_t n);

/* State moved out of the emulated memory (docs/refactor.md, R1). A range of the
 * data area that a C object holds is an alias: x86_ptr hands out the object's
 * bytes for it, so the emulated code, the HLE layer and bulk copies see the
 * same bytes at the same address while the C code names the fields. Within
 * [X86_ALIAS_LO, X86_ALIAS_HI) only; an access must lie wholly in one alias
 * or wholly outside all of them. */
#define X86_ALIAS_LO 0x431000u
#define X86_ALIAS_HI 0x4c0000u
uint8_t *x86_alias_ptr(uint32_t a, uint32_t n);
/* set when one object backs the whole range: the fast path of x86_ptr */
extern uint8_t *x86_data_obj;
/* Make [addr, addr + size) the bytes of obj; obj takes the memory's current
 * content. */
void x86_alias(uint32_t addr, void *obj, uint32_t size);
void x86_unaliased_snapshot(void);
int x86_unaliased_check(void); /* the number of changed ranges */
/* Copy n bytes of the emulated address space (aliases included) out */
void x86_read_virtual(uint32_t a, void *dst, uint32_t n);

static inline uint8_t *x86_ptr(uint32_t a, uint32_t n)
{
	if (a < MEM_BASE || a + n > MEM_END || a + n < a)
		x86_fault(a);
	if (__builtin_expect(x86_img_map != 0, 0) && a - MEM_BASE < IMG_SIZE)
		x86_img_mark(a, n);
	if (__builtin_expect(x86_data_obj != 0, 1) && a >= X86_ALIAS_LO && a + n <= X86_ALIAS_HI)
		return x86_data_obj + (a - X86_ALIAS_LO);
	if (a < X86_ALIAS_HI && a + n > X86_ALIAS_LO)
		return x86_alias_ptr(a, n);
	return x86_mem + (a - MEM_BASE);
}

static inline uint32_t rd8(uint32_t a) { return *x86_ptr(a, 1); }
static inline uint32_t rd16(uint32_t a) { uint16_t v; memcpy(&v, x86_ptr(a, 2), 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, x86_ptr(a, 4), 4); return v; }
static inline void wr8(uint32_t a, uint32_t v) { *x86_ptr(a, 1) = (uint8_t)v; }
static inline void wr16(uint32_t a, uint32_t v) { uint16_t t = (uint16_t)v; memcpy(x86_ptr(a, 2), &t, 2); }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(x86_ptr(a, 4), &v, 4); }

static inline void push32(uint32_t v) { cpu.r[ESP] -= 4; wr32(cpu.r[ESP], v); }
static inline uint32_t pop32(void) { uint32_t v = rd32(cpu.r[ESP]); cpu.r[ESP] += 4; return v; }

/* the one arithmetic fault the script expressions can reach (div by zero) */
void x86_divide_error(void);

/* ---------------------------------------------------------------- host */

/* Called by lifted code on backward jumps: runs due timer ticks and
 * presents the screen, as the native timer thread and DirectDraw would. */
extern int x86_poll_countdown;
extern uint32_t x86_poll_pc;
void x86_poll(void);
#define X86_POLL(pc) do { if (--x86_poll_countdown <= 0) { x86_poll_pc = (pc); x86_poll(); } } while (0)

/* Emulated CPU speed. Lifted code adds each basic block's instruction count
 * (rep string ops: one per iteration) to x86_icount; x86_poll sleeps when the
 * count runs ahead of real time at the configured rate (win32.c). */
extern uint64_t x86_icount;
#define X86_COUNT(n) (x86_icount += (n))

/* Instructions per second; 0 = unthrottled. */
void x86_set_speed(double ips);
void x86_throttle(void);

/* Timing probe run before selected instructions (src/hle/overrides.h
 * HLE_PROBE); logs a timestamp with NANPA2_PROBE=1. */
void x86_probe(uint32_t pc);

/* Load the exe image into emulated memory: 1 ok, 0 cannot read, -1 not
 * the nanpa2.exe this engine runs (wrong file or another build). */
int x86_load_image(const char *exe_path);
/* The exe image without installing it (src/x86/asset.c cuts the asset from
 * it): a fresh MEM_END - MEM_BASE bytes, or NULL with *status 0 (cannot
 * read) or -1 (not this nanpa2.exe). hash and exe_size may be NULL. */
uint8_t *x86_image_read(const char *exe_path, int *status, uint64_t *hash, uint32_t *exe_size);
/* Make mem (from x86_image_read or x86_asset_read) the emulated memory. */
void x86_install_image(uint8_t *mem);
uint64_t x86_fnv64(const void *p, size_t n, uint64_t h);
#define X86_FNV64_INIT 0xcbf29ce484222325ull

/* nanpa2.dat, the built-in data of the exe without its code (asset.c).
 * x86_asset_make: 1 written, 0 io error, -1 exe is not this build.
 * x86_asset_read: 1 loaded and installed, 0 cannot read, -1 not an asset of
 * this format and build. After it x86_code_absent is set: game code bytes
 * are not in memory, so the interpreter's decoder refuses to run. */
int x86_asset_make(const char *exe_path, const char *out_path);
int x86_asset_read(const char *path);
extern int x86_code_absent;

/* 1 if the first n bytes of a file are the header of that nanpa2.exe */
int x86_exe_check(const uint8_t *d, size_t n);
/* Emulated heap */
/* end of the used heap (replay checkpoints hash HEAP_BASE up to it) */
uint32_t x86_heap_top(void);
uint32_t x86_malloc(uint32_t size);
void x86_free(uint32_t addr);

#endif
