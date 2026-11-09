#include "hle.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/game.h"
#include "logo.h"
#include "lzh.h"
#include "x86/interp.h"
#include "game/state.h"

#define MAX_IMPORTS 256

static struct {
	char name[64];
	int nargs;     /* stdcall arguments to pop */
	int id;        /* win32.c function id, -1 if unknown */
} imports[MAX_IMPORTS];
static int nimports;

void hle_strcpy_in(char *dst, uint32_t src, size_t cap)
{
	size_t i = 0;
	if (cap == 0)
		return;
	if (src)
		for (; i + 1 < cap; i++) {
			uint32_t c = rd8(src + (uint32_t)i);
			if (!c)
				break;
			dst[i] = (char)c;
		}
	dst[i] = 0;
}

uint32_t hle_strlen(uint32_t a)
{
	uint32_t n = 0;
	while (rd8(a + n))
		n++;
	return n;
}

/* Walk the import directory and point every IAT slot at a thunk. */
static int map_imports(void)
{
	uint32_t pe = MEM_BASE + rd32(MEM_BASE + 0x3c);
	uint32_t dir = rd32(pe + 0x80);
	if (!dir)
		return 0;
	for (uint32_t d = MEM_BASE + dir; rd32(d + 12); d += 20) {
		char dll[64];
		hle_strcpy_in(dll, MEM_BASE + rd32(d + 12), sizeof dll);
		uint32_t ilt = rd32(d) ? rd32(d) : rd32(d + 16);
		uint32_t iat = MEM_BASE + rd32(d + 16);
		for (uint32_t k = 0;; k++) {
			uint32_t v = rd32(MEM_BASE + ilt + 4 * k);
			if (!v)
				break;
			if (nimports >= MAX_IMPORTS)
				return 0;
			char name[64];
			if (v & 0x80000000u)
				snprintf(name, sizeof name, "#%u", v & 0xffff);
			else
				hle_strcpy_in(name, MEM_BASE + v + 2, sizeof name);
			int known = 0;
			imports[nimports].id = (int)win32_import(dll, name, &imports[nimports].nargs, &known);
			if (!known)
				imports[nimports].id = -1;
			snprintf(imports[nimports].name, sizeof imports[nimports].name, "%s!%s", dll, name);
			wr32(iat + 4 * k, THUNK_IMPORT + 16u * (uint32_t)nimports);
			nimports++;
		}
	}
	return 1;
}

int hle_init(const char *game_dir, const char *save_dir)
{
	x86_interp_init(); /* the override list, used by both backends */
	crt_init(game_dir, save_dir);
	win32_init();
	return map_imports();
}

/* Everything outside the game code (thunks, the C runtime) and the
 * overrides of overrides.h, the list both backends use. */
int hle_is_target(uint32_t addr)
{
	return x86_is_hle(addr);
}

/* ---- COM objects ----
 * An object is [vtable pointer][fields...] in emulated memory; the vtable
 * holds thunk addresses. */

uint32_t com_vtable(int iface, int nmethods)
{
	static uint32_t tables[16];
	if (!tables[iface]) {
		tables[iface] = x86_malloc(4u * (uint32_t)nmethods);
		for (int i = 0; i < nmethods; i++)
			wr32(tables[iface] + 4u * (uint32_t)i,
			     THUNK_COM + 0x100u * (uint32_t)iface + 4u * (uint32_t)i);
	}
	return tables[iface];
}

uint32_t com_new(int iface, int nmethods, uint32_t extra_bytes)
{
	uint32_t o = x86_malloc(4 + extra_bytes);
	wr32(o, com_vtable(iface, nmethods));
	return o;
}

/* ---- lifted functions replaced by hand ---- */

/* native 0x4028c0: 16 ms multimedia timer callback. Steps a palette fade
 * (start colours at 0x4aac58, deltas at 0x4ab060, step 0x4ac508 of 16,
 * every 0x44628e ticks), counts frames at 0x446292, and blinks the
 * software cursor every 30 ticks. */
void hle_timer_callback(void)
{
	static uint32_t step_logged_start = 0xffffffffu;
	if (!SC(active))
		return;
	if (SC(fade_active)) {
		if (step_logged_start != RT(frame) && SC(fade_step) == 0 && SC(fade_wait) == 0)
			x86_probe(0x4028c0); /* fade starts (probe log) */
		step_logged_start = RT(frame);
		uint32_t wait = SC(fade_wait) + 1;
		SC_SET(fade_wait, wait);
		if ((int32_t)wait >= (int32_t)RT(fade_steps)) {
			SC_SET(fade_wait, 0);
			float k;
			uint32_t kbits = rd32(0x41b188);
			memcpy(&k, &kbits, 4);
			double f = (double)(int32_t)SC(fade_step) * k;
			for (uint32_t i = 0; i < 256; i++) {
				for (uint32_t c = 0; c < 3; c++) {
					int32_t delta = G.sc.pal_delta[i][c];
					double v = delta * f + G.sc.pal_start[4 * i + c];
					G.sc.pal_entries[4 * i + c] = (uint8_t)(int32_t)v; /* _ftol truncates */
				}
			}
			ddraw_set_entries(0, 256, 0x4ac07c);
		}
		uint32_t step = SC(fade_step) + 1;
		SC_SET(fade_step, step);
		if ((int32_t)step > 16) {
			SC_SET(fade_wait, 0);
			SC_SET(fade_step, 0);
			SC_SET(fade_active, 0);
			x86_probe(0x4028c1); /* fade ends (probe log) */
		}
	}
	uint32_t frame = RT(frame) + 1;
	RT_SET(frame, frame);
	if (SC(cursor_on) && (int32_t)frame % 30 == 0) {
		SC_SET(cursor_frame, SC(cursor_frame) ^ 1);
		win32_post_message(0x200, 0, (SC(cursor_y) << 16) + SC(cursor_x));
	}
}

static void override_call(uint32_t addr)
{
	switch (addr) {
	case 0x401bf0: /* message pump */
		cpu.r[EAX] = (uint32_t)win32_pump();
		/* The native pump returns from PeekMessageA/DispatchMessageA,
		 * which leave ECX changed. Script op 0x1f (call a MES file,
		 * 0x406fcd) relies on that: after the sub-file returns
		 * through this pump it restores its state with
		 * "mov cx, 0x1e / rep movsd" (0x40705e, 0x40706e), which loads
		 * only CX. A stale heap pointer in ECX made that copy run off
		 * memory (ecx 0x0104001e). */
		cpu.r[ECX] = 0;
		cpu.r[EDX] = 0; /* also volatile across the stdcall APIs */
		return;
	case 0x401e20: /* LOGO.AVI through MCI (src/hle/logo.c) */
		win32_show_cursor(0); /* 0x401e27, before anything else */
		logo_play();
		return;
	case 0x402ad0: /* DirectSound init */
		cpu.r[EAX] = (uint32_t)sound_init();
		return;
	case 0x402c70: /* sound shutdown: voice stop, music stop, release */
		sound_shutdown();
		return;
	/* Internals of the native DirectSound path. Their only callers are
	 * the overrides here (0x402cb0, 0x403250, 0x403360, 0x4034a0) and
	 * 0x403020/0x4030f0, which only those call, so the lifted code never
	 * reaches them; they are listed so a stray call cannot run native
	 * DirectSound code. */
	case 0x402f10: { /* cdecl (wav, &data): size of the WAV "data" chunk */
		uint32_t wav = hle_arg(0), out = hle_arg(1);
		uint32_t at = rd32(wav + 0x10) + 0x14; /* skip RIFF/WAVE/fmt chunk */
		for (int i = 0; i < 64 && rd32(wav + at) != 0x61746164u; i++)
			at += 8 + rd32(wav + at + 4);
		if (out)
			wr32(out, wav + at + 8);
		cpu.r[EAX] = rd32(wav + at + 4);
		return;
	}
	case 0x402f60: /* Lock/copy/Unlock a DirectSound buffer: no buffers */
	case 0x4031d0: /* decode MP3 into the stream buffer: sound.c decodes */
		return;
	case 0x402cb0: { /* play MUSIC\%02d.MP3 (or .WAV) */
		sound_music_play((int)hle_arg(0));
		return;
	}
	case 0x403360: { /* play voice "SPEECH%s" */
		char name[128], rel[160];
		hle_strcpy_in(name, hle_arg(0), sizeof name);
		snprintf(rel, sizeof rel, "SPEECH%s", name);
		sound_voice_play(rel);
		return;
	}
	case 0x403220: /* WM_ACTIVATEAPP off: pause music */
		sound_music_pause(1);
		return;
	case 0x403250: /* WM_ACTIVATEAPP on: resume music */
		sound_music_pause(0);
		return;
	case 0x4032c0: /* music stop */
		sound_music_stop();
		return;
	case 0x403440: /* voice stop */
		sound_voice_stop();
		return;
	case 0x4034a0: /* music stream pump: SDL audio pulls instead */
		return;
	case 0x4075f0: { /* decompress(dst, src): src is a malloc'd file image */
		/* native 0x401050 reads the whole file before calling; the size is
		 * the heap block size, which is enough for the bit reader */
		uint32_t dst = hle_arg(0), src = hle_arg(1);
		uint32_t cap = rd32(src - 8) - 8;
		size_t n;
		unsigned char *out = lzh_decode(x86_ptr(src, cap), cap, &n);
		if (!out)
			x86_trap(addr, "corrupt GP4/ANI data");
		memcpy(x86_ptr(dst, (uint32_t)n), out, n);
		/* emulated CPU time of the native decoder: lifting it and counting
		 * gave about 70 instructions per input byte + 1.2 per output byte
		 * (0.7M for ELF.GP4, 2.4M for NAME.GP4) */
		X86_COUNT(70u * (uint64_t)cap + (uint64_t)n * 6 / 5);
		free(out);
		return;
	}
	}
	char msg[64];
	snprintf(msg, sizeof msg, "no HLE for %08x", addr);
	x86_trap(addr, msg);
}

uint32_t hle_jump(uint32_t addr)
{
	uint32_t pc;
	if (game_jump(addr, &pc))
		return pc;
	char msg[64];
	snprintf(msg, sizeof msg, "no jump override for %08x", addr);
	x86_trap(addr, msg);
}

/* NANPA2_HLETRACE=1: every call into the HLE layer (imports, COM methods,
 * C runtime, HLE overrides) with its first six arguments, before it runs.
 * Stack addresses are written STK so that two builds' traces compare (the
 * start-up check of docs/reimpl.md, Stage 5B). */
static void hle_trace(uint32_t addr)
{
	static int on = -1;
	if (on < 0) {
		const char *e = getenv("NANPA2_HLETRACE");
		on = !e ? 0 : !strcmp(e, "2") ? 2 : 1; /* 2: with the instruction count */
	}
	if (!on)
		return;
	if (on == 2)
		fprintf(stderr, "ic %llu ", (unsigned long long)x86_icount);
	fprintf(stderr, "hle %08x", addr);
	int n = 1; /* arguments known: imports have their count, others show the first */
	if (addr >= THUNK_IMPORT && addr < THUNK_COM) {
		int k = (int)((addr - THUNK_IMPORT) / 16);
		n = k < nimports ? imports[k].nargs : 1;
	}
	for (int i = 1; i <= n; i++) {
		uint32_t v = rd32(cpu.r[ESP] + 4u * (uint32_t)i);
		if (v >= 0x00f00000u && v < 0x01000000u)
			fprintf(stderr, " STK");
		else
			fprintf(stderr, " %x", v);
	}
	fputc('\n', stderr);
}

/* the trace line of a pump that the C engine calls directly (ctl.c) */
void hle_trace_pump(void)
{
	hle_trace(0x401bf0);
}

void hle_call(uint32_t addr)
{
	hle_trace(addr);
	pop32(); /* return address: the caller continues there */
	if (addr >= THUNK_COM && addr < THUNK_END) {
		uint32_t iface = (addr - THUNK_COM) >> 8, method = ((addr - THUNK_COM) & 0xff) >> 2;
		if (ddraw_com_call(iface, method))
			return;
		char msg[64];
		snprintf(msg, sizeof msg, "COM interface %u method %u", iface, method);
		x86_trap(addr, msg);
	}
	if (addr >= THUNK_IMPORT && addr < THUNK_COM) {
		int i = (int)((addr - THUNK_IMPORT) / 16);
		static int log = -1;
		if (log < 0)
			log = getenv("NANPA2_LOG") != NULL;
		if (log && i < nimports)
			fprintf(stderr, "call %s\n", imports[i].name);
		if (i < nimports && imports[i].id >= 0 && win32_call_import(imports[i].id)) {
			hle_stdcall_pop(imports[i].nargs);
			return;
		}
		char msg[96];
		snprintf(msg, sizeof msg, "import %s", i < nimports ? imports[i].name : "?");
		x86_trap(addr, msg);
	}
	if (game_call(addr)) /* reimplemented game logic (src/game) */
		return;
	if (crt_call(addr))
		return;
	override_call(addr);
}
