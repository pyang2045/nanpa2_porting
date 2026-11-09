/*
 * nanpa2.dat: the built-in data of the player's nanpa2.exe, without its code.
 *
 * The engine reimplements every game function in C (docs/reimpl.md), but the
 * C still reads the exe's initialized data at its native addresses through
 * the vm.h accessors: tables, strings, the import directory and the globals'
 * start values. This file cuts exactly those ranges out of the player's exe
 * once (nanpa2 --make-assets, or on a first launch) so that the game runs
 * without the exe. It is generated on the player's machine and never shipped
 * or committed (.gitignore); this source holds the format and the list of
 * address ranges to keep, no bytes of the exe.
 *
 * Format (little-endian), re-notes.md "nanpa2.dat":
 *   0   "N2AS"
 *   4   u32 version (1)
 *   8   u32 PE TimeDateStamp of the source exe (0x39697c6a)
 *   12  u32 SizeOfImage of the source exe (0xc1000)
 *   16  u32 size of the source exe file
 *   20  u32 number of ranges
 *   24  u64 FNV-1a of the source exe file
 *   32  u64 FNV-1a of everything after this 48-byte header
 *   40  u32 payload bytes
 *   44  u32 0
 *   48  ranges: { u32 virtual address, u32 size, u32 kind } each;
 *       kind 0 = bytes in the payload, kind 1 = zeros (not stored)
 *   ... payload: the kind 0 ranges' bytes in order
 * The loader maps each range at its address in a zeroed image; addresses not
 * listed (the code, the MP3 library, the C runtime, the resources, the bss)
 * stay zero.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x86.h"

#define HDR 48u
#define MIN_ZERO 32u /* shorter zero runs stay inside a data range */

/* The ranges of the image kept: the PE header page, and .rdata with the
 * initialized part of .data. Left out: .text (0x401000-0x41b000: the game
 * code 0x401000-0x4077ac, the MP3 library, the C runtime), the bss tail of
 * .data (zero at load) and .rsrc. Established by NANPA2_IMAGE_TRACE runs
 * (docs/reimpl.md "The asset"). */
static const struct { uint32_t lo, hi; } keep[] = {
	{ 0x400000, 0x401000 },
	{ 0x41b000, 0x4ab000 },
};

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
	return get32(p) | (uint64_t)get32(p + 4) << 32;
}

struct seg { uint32_t va, size, kind; };

/* Length of the zero run at a, up to end. */
static uint32_t zrun(const uint8_t *img, uint32_t a, uint32_t end)
{
	uint32_t z = a;
	while (z < end && img[z - MEM_BASE] == 0)
		z++;
	return z - a;
}

/* Split the kept ranges into data and zero runs (of MIN_ZERO or more) of
 * the image. */
static int split(const uint8_t *img, struct seg **out)
{
	size_t cap = 64, n = 0;
	struct seg *s = malloc(cap * sizeof *s);
	for (size_t k = 0; k < sizeof keep / sizeof keep[0]; k++) {
		uint32_t a = keep[k].lo, end = keep[k].hi;
		while (a < end) {
			uint32_t b = a, zl = zrun(img, a, end);
			int zero = zl >= MIN_ZERO;
			if (zero) {
				b = a + zl;
			} else {
				while (b < end) {
					if (img[b - MEM_BASE]) {
						b++;
						continue;
					}
					zl = zrun(img, b, end);
					if (zl >= MIN_ZERO || b + zl == end)
						break;
					b += zl;
				}
				if (b == a)
					b = a + zl; /* a short zero tail */
			}
			if (n == cap)
				s = realloc(s, (cap *= 2) * sizeof *s);
			s[n++] = (struct seg){ a, b - a, zero ? 1u : 0u };
			a = b;
		}
	}
	*out = s;
	return (int)n;
}

int x86_asset_make(const char *exe_path, const char *out_path)
{
	int st;
	uint64_t exe_hash = 0;
	uint32_t exe_size = 0;
	uint8_t *img = x86_image_read(exe_path, &st, &exe_hash, &exe_size);
	if (!img)
		return st;
	struct seg *seg;
	int n = split(img, &seg);
	uint32_t payload = 0;
	for (int i = 0; i < n; i++) {
		/* the game code must never be in the asset */
		if (seg[i].va < CODE_END && seg[i].va + seg[i].size > CODE_START) {
			free(seg);
			free(img);
			return 0;
		}
		if (!seg[i].kind)
			payload += seg[i].size;
	}
	size_t total = HDR + 12u * (size_t)n + payload;
	uint8_t *buf = calloc(1, total);
	if (!buf) {
		free(seg);
		free(img);
		return 0;
	}
	uint8_t *tab = buf + HDR, *pay = tab + 12u * (size_t)n;
	for (int i = 0; i < n; i++) {
		put32(tab + 12 * i, seg[i].va);
		put32(tab + 12 * i + 4, seg[i].size);
		put32(tab + 12 * i + 8, seg[i].kind);
		if (!seg[i].kind) {
			memcpy(pay, img + (seg[i].va - MEM_BASE), seg[i].size);
			pay += seg[i].size;
		}
	}
	memcpy(buf, "N2AS", 4);
	put32(buf + 4, 1);
	put32(buf + 8, 0x39697c6au);
	put32(buf + 12, 0xc1000u);
	put32(buf + 16, exe_size);
	put32(buf + 20, (uint32_t)n);
	put64(buf + 24, exe_hash);
	put32(buf + 40, payload);
	put64(buf + 32, x86_fnv64(buf + HDR, total - HDR, X86_FNV64_INIT));
	free(seg);
	free(img);

	char tmp[1200];
	snprintf(tmp, sizeof tmp, "%s.tmp", out_path);
	FILE *f = fopen(tmp, "wb");
	int ok = f && fwrite(buf, 1, total, f) == total;
	if (f && fclose(f))
		ok = 0;
	free(buf);
#ifdef _WIN32
	if (ok)
		remove(out_path); /* rename does not replace there */
#endif
	if (ok && rename(tmp, out_path))
		ok = 0;
	if (!ok)
		remove(tmp);
	return ok;
}

int x86_asset_read(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = len > 0 ? malloc((size_t)len) : NULL;
	if (!d || fread(d, 1, (size_t)len, f) != (size_t)len) {
		fclose(f);
		free(d);
		return 0;
	}
	fclose(f);
	size_t total = (size_t)len;
	int bad = total < HDR || memcmp(d, "N2AS", 4) || get32(d + 4) != 1 ||
		  get32(d + 8) != 0x39697c6au || get32(d + 12) != 0xc1000u;
	uint32_t n = bad ? 0 : get32(d + 20), payload = bad ? 0 : get32(d + 40);
	if (!bad && (n > 4096 || HDR + 12ull * n + payload != total ||
		     x86_fnv64(d + HDR, total - HDR, X86_FNV64_INIT) != get64(d + 32)))
		bad = 1;
	uint8_t *mem = bad ? NULL : calloc(1, MEM_END - MEM_BASE);
	if (!mem) {
		free(d);
		return bad ? -1 : 0;
	}
	const uint8_t *tab = d + HDR, *pay = tab + 12u * n;
	size_t left = payload;
	for (uint32_t i = 0; i < n && !bad; i++) {
		uint32_t va = get32(tab + 12 * i), size = get32(tab + 12 * i + 4), kind = get32(tab + 12 * i + 8);
		if (va < MEM_BASE || va - MEM_BASE > IMG_SIZE || size > IMG_SIZE - (va - MEM_BASE) || kind > 1 ||
		    (va < CODE_END && va + size > CODE_START) || (!kind && size > left)) {
			bad = 1;
			break;
		}
		if (!kind) {
			memcpy(mem + (va - MEM_BASE), pay, size);
			pay += size;
			left -= size;
		}
	}
	free(d);
	if (bad || left) {
		free(mem);
		return -1;
	}
	x86_code_absent = 1;
	x86_install_image(mem);
	return 1;
}
