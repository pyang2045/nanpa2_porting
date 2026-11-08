/*
 * Stage 5B: file access. Opening a game file 0x401000, loading a whole file
 * 0x401050 and saving one 0x401160.
 *
 * A name is looked up under DATA\ (unless it starts with "DATA" already),
 * first as given and then under the CD prefix the start-up search found
 * (0x431030). A file whose name ends in .GP4 or .ANI is read whole into a
 * heap block and decompressed into the caller's buffer (the HLE 0x4075f0,
 * src/lzh.c); anything else is read straight into the buffer. A file that
 * cannot be opened is silently skipped. The C runtime (sprintf, fopen,
 * fread, ...) is the replacement in src/hle/crt.c, called by its native
 * address through game_ncall so the harness traces the bytes fread writes.
 * Semantics: re-notes.md "File access".
 */
#include "game/file.h"
#include "game/vm.h"
#include "x86/x86.h"

#define CD_PREFIX 0x431030u   /* string: where the CD search found the game's CD */
#define FMT_PREFIXED 0x431034u /* "%s%s": prefix and name */
#define MODE_READ 0x43103cu   /* "rb" */
#define EXT_ANI 0x431040u
#define EXT_GP4 0x431048u
#define FMT_DATA 0x431050u    /* "DATA\%s" */
#define NAME_DATA 0x431058u   /* "DATA", compared with the name's start */
#define MODE_WRITE 0x431060u  /* "wb" */

#define C_SPRINTF 0x412c90u
#define C_FOPEN 0x412d13u
#define C_FCLOSE 0x412d6eu
#define C_FREAD 0x412debu
#define C_FWRITE 0x412fb7u
#define C_FREE 0x412d26u
#define C_MALLOC 0x412f02u
#define C_STRRCHR 0x412f90u
#define C_FILENO 0x41a730u
#define C_FILELENGTH 0x41a6a6u
#define C_STRICMP 0x41a3d0u
#define C_STRNICMP 0x41a740u
#define DECOMPRESS 0x4075f0u  /* cdecl (dst, src): GP4/ANI, HLE */

/* a cdecl call of a C runtime routine: the arguments are pushed last
 * first, the caller pops them. Charges the pushes, the call and the
 * `add esp` (callers fold several pops into one: pass pops = 0 there). */
static uint32_t crt(uint32_t addr, int pops, int nargs, const uint32_t *args)
{
	for (int i = nargs - 1; i >= 0; i--)
		push32(args[i]);
	X86_COUNT((uint64_t)nargs + 1);
	game_ncall(addr, 0);
	cpu.r[ESP] += 4u * (uint32_t)nargs;
	if (pops)
		X86_COUNT(1);
	return cpu.r[EAX];
}

#define CRT(addr, pops, ...) crt((addr), (pops), (int)(sizeof((uint32_t[]){ __VA_ARGS__ }) / 4), \
	(const uint32_t[]){ __VA_ARGS__ })

/* 0x401000: fopen(name, "rb"); if that fails, fopen(CD prefix + name).
 * Returns the stream (0: none) in EAX. `esp_in` is the stack pointer a
 * native `call` would find (the return address goes below it): the
 * 40-byte buffer is the native one's address, and after a retry ECX is
 * that address, which the callers' register effects show. */
static uint32_t file_open_at(uint32_t name, uint32_t esp_in)
{
	uint32_t esp = cpu.r[ESP], buf = esp_in - 4 - 0x28;
	cpu.r[ESP] = buf;
	X86_COUNT(3); /* sub esp, push esi, mov esi */
	uint32_t f = CRT(C_FOPEN, 1, name, MODE_READ);
	X86_COUNT(2); /* test, jne */
	if (!f) {
		CRT(C_SPRINTF, 0, buf, FMT_PREFIXED, CD_PREFIX, name);
		cpu.r[ECX] = buf;
		X86_COUNT(2); /* lea, lea */
		f = CRT(C_FOPEN, 0, buf, MODE_READ);
		X86_COUNT(1); /* add esp, 0x18 */
	}
	X86_COUNT(3); /* pop esi, add esp, ret */
	cpu.r[ESP] = esp;
	cpu.r[EAX] = f;
	return f;
}

uint32_t game_file_open(uint32_t name)
{
	return file_open_at(name, cpu.r[ESP]);
}

/* 0x401050: load the file `name` into `buf` (see above). Leaves EDX = the
 * path buffer and ECX = buf after a plain read (or the path / the retry
 * buffer / 0 otherwise), as the native code does. */
void game_file_load(uint32_t name, uint32_t buf)
{
	uint32_t esp = cpu.r[ESP];
	uint32_t path = esp - 4 - 0x28; /* the native local_28 */
	cpu.r[ESP] = path;
	X86_COUNT(6); /* sub esp, push ebx, ebp, esi, edi; mov ebp, name */
	if (CRT(C_STRNICMP, 1, name, NAME_DATA, 4) == 0) {
		/* already DATA\...: copy the name as it is (strlen, rep movs) */
		uint32_t n = 0;
		while (rd8(name + n))
			n++;
		n++;
		for (uint32_t i = 0; i < n; i++)
			wr8(path + i, rd8(name + i));
		cpu.r[ECX] = 0;
		X86_COUNT(2 + 16 + n + n / 4 + (n & 3));
	} else {
		cpu.r[ECX] = path;
		CRT(C_SPRINTF, 1, path, FMT_DATA, name);
		X86_COUNT(1);
	}
	X86_COUNT(2); /* the test/jne of the strnicmp */
	cpu.r[EDX] = path;
	/* the call of 0x401000: its argument below the 4 saved registers and
	 * the local buffer */
	uint32_t f = file_open_at(path, path - 0x10 - 4);
	X86_COUNT(7); /* lea, push, call; mov, add, test, je */
	if (f) {
		uint32_t fd = CRT(C_FILENO, 0, f);
		uint32_t size = CRT(C_FILELENGTH, 0, fd);
		X86_COUNT(2);
		uint32_t dot = CRT(C_STRRCHR, 1, name, 0x2e);
		X86_COUNT(2); /* test, je */
		int packed = 0;
		if (dot) {
			packed = CRT(C_STRICMP, 1, dot, EXT_GP4) == 0;
			X86_COUNT(2);
			if (!packed) {
				packed = CRT(C_STRICMP, 1, dot, EXT_ANI) == 0;
				X86_COUNT(2);
			}
		}
		if (packed) {
			uint32_t img = CRT(C_MALLOC, 0, size);
			CRT(C_FREAD, 0, img, 1, size, f);
			CRT(C_FCLOSE, 0, f);
			X86_COUNT(2); /* mov edi, mov eax */
			CRT(DECOMPRESS, 0, buf, img);
			CRT(C_FREE, 0, img);
			X86_COUNT(7); /* add esp, pops, ret */
		} else {
			cpu.r[ECX] = buf;
			CRT(C_FREAD, 0, buf, 1, size, f);
			CRT(C_FCLOSE, 0, f);
			X86_COUNT(2 + 6); /* mov ecx; add esp; pops, ret */
		}
	} else {
		X86_COUNT(6);
	}
	cpu.r[ESP] = esp;
}

/* 0x401160: write `size` bytes at `buf` to the file `name` ("wb"; the file
 * name is used as given, no DATA\ prefix). Leaves ECX = size, EDX = buf. */
void game_file_save(uint32_t name, uint32_t buf, uint32_t size)
{
	X86_COUNT(2); /* mov eax, push esi */
	uint32_t f = CRT(C_FOPEN, 0, name, MODE_WRITE);
	X86_COUNT(2); /* mov ecx, mov edx, mov esi */
	CRT(C_FWRITE, 0, buf, 1, size, f);
	cpu.r[ECX] = size;
	cpu.r[EDX] = buf;
	CRT(C_FCLOSE, 0, f);
	X86_COUNT(4); /* add esp, pop esi, ret */
}

/* register-level entries (cdecl: arguments on the stack) */
void game_file_open_regs(void)
{
	game_file_open(rd32(cpu.r[ESP]));
}

void game_file_load_regs(void)
{
	game_file_load(rd32(cpu.r[ESP]), rd32(cpu.r[ESP] + 4));
}

void game_file_save_regs(void)
{
	game_file_save(rd32(cpu.r[ESP]), rd32(cpu.r[ESP] + 4), rd32(cpu.r[ESP] + 8));
}
