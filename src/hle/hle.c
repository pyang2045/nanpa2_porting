#include "hle.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logo.h"
#include "lzh.h"

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
	crt_init(game_dir, save_dir);
	win32_init();
	return map_imports();
}

int hle_is_target(uint32_t addr)
{
	if (addr >= THUNK_IMPORT && addr < THUNK_END)
		return 1;
	for (const uint32_t *t = x86_hle_targets; *t; t++)
		if (*t == addr)
			return 1;
	return 0;
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
	if (!rd32(0x4ac4ec))
		return;
	if (rd32(0x4ac504)) {
		uint32_t wait = rd32(0x4ac50c) + 1;
		wr32(0x4ac50c, wait);
		if ((int32_t)wait >= (int32_t)rd32(0x44628e)) {
			wr32(0x4ac50c, 0);
			float k;
			uint32_t kbits = rd32(0x41b188);
			memcpy(&k, &kbits, 4);
			double f = (double)(int32_t)rd32(0x4ac508) * k;
			for (uint32_t i = 0; i < 256; i++) {
				for (uint32_t c = 0; c < 3; c++) {
					int32_t delta = (int32_t)rd32(0x4ab060 + 12 * i + 4 * c);
					double v = delta * f + rd8(0x4aac58 + 4 * i + c);
					wr8(0x4ac07c + 4 * i + c, (uint32_t)(int32_t)v); /* _ftol truncates */
				}
			}
			ddraw_set_entries(0, 256, 0x4ac07c);
		}
		uint32_t step = rd32(0x4ac508) + 1;
		wr32(0x4ac508, step);
		if ((int32_t)step > 16) {
			wr32(0x4ac50c, 0);
			wr32(0x4ac508, 0);
			wr32(0x4ac504, 0);
		}
	}
	uint32_t frame = rd32(0x446292) + 1;
	wr32(0x446292, frame);
	if (rd32(0x4ac510) && (int32_t)frame % 30 == 0) {
		wr32(0x4ac514, rd32(0x4ac514) ^ 1);
		win32_post_message(0x200, 0, (rd32(0x4ac060) << 16) + rd32(0x4ac064));
	}
}

static void override_call(uint32_t addr)
{
	switch (addr) {
	case 0x401bf0: /* message pump */
		cpu.r[EAX] = (uint32_t)win32_pump();
		return;
	case 0x401e20: /* LOGO.AVI through MCI (src/hle/logo.c) */
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
		free(out);
		return;
	}
	}
	char msg[64];
	snprintf(msg, sizeof msg, "no HLE for %08x", addr);
	x86_trap(addr, msg);
}

void hle_call(uint32_t addr)
{
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
	if (crt_call(addr))
		return;
	override_call(addr);
}
