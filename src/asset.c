/*
 * nanpa2.dat: the built-in data of the player's nanpa2.exe, without its code (R4: this file; it was
 * src/x86/asset.c together with the exe image loader of src/x86/x86.c).
 *
 * The engine reimplements every game function in C (docs/reimpl.md) and defines the start values
 * of its game state G in code (src/game/state.c). What it cannot carry in its source is the game's
 * own text (the no-CD message and one more short text): this file cuts exactly those ranges out of
 * the player's exe once (nanpa2 --make-assets, or on a first launch). It is generated on the
 * player's machine and never shipped or committed (.gitignore); this source holds the format and
 * the list of address ranges to keep, no bytes of the exe.
 *
 * Format (little-endian), re-notes.md "nanpa2.dat":
 *   0   "N2AS"
 *   4   u32 version (3: only the ranges of asset_content; 1 and 2 held more and are still read)
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

#include "asset.h"

#define HDR 48u
#define ASSET_VERSION 3u
#define MIN_ZERO 32u /* shorter zero runs stay inside a data range */

/* The ranges of the image kept: the game's own text that G's initial state holds and the engine
 * must not carry in its source (R4: asset version 3). The rest of G's initial state is defined in
 * code (src/game/state.c state_defaults); everything else of the exe is read by nothing: the PE
 * header, .text (the game code 0x401000-0x4077ac, the MP3 library, the C runtime), .rdata and
 * .rsrc, and the pointers and strings of .data that only the native code used. Version 1 kept the
 * header page and .rdata 0x41b000-0x4ab000, version 2 the whole initialised .data
 * 0x431000-0x4ab000; both are still read (state_init takes only these ranges from them). */
const struct asset_range asset_content[] = {
	{ 0x431140, 0x431160 }, /* the message of the no-CD box and its title */
	{ 0x445c70, 0x445c78 }, /* a 7-byte text in the script-visible block */
};
const unsigned asset_content_n = sizeof asset_content / sizeof asset_content[0];

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

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t le16(const uint8_t *p)
{
	return p[0] | p[1] << 8;
}

int asset_exe_check(const uint8_t *d, size_t n)
{
	if (n < 0x400 || le32(d + 0x3c) > n - 0x100)
		return 0;
	uint32_t pe = le32(d + 0x3c);
	/* the Taiwan release built 2000-07-10 (PE TimeDateStamp 0x39697c6a,
	 * SizeOfImage 0xc1000): the native addresses throughout the engine
	 * are this build's */
	return le32(d + pe) == 0x4550u && le32(d + pe + 52) == IMAGE_BASE &&
	       le32(d + pe + 8) == 0x39697c6au && le32(d + pe + 80) == 0xc1000u;
}

/* The exe's sections laid out as the loader would, in a fresh zeroed image
 * of IMAGE_SIZE bytes. NULL with *status 0: cannot read; -1: not the
 * nanpa2.exe this engine runs. *hash is the FNV-1a of the file. */
uint8_t *asset_image_read(const char *exe_path, int *status, uint64_t *hash, uint32_t *exe_size)
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
	if (!asset_exe_check(d, (size_t)n)) {
		free(d);
		*status = -1;
		return NULL;
	}
	uint8_t *mem = calloc(1, IMAGE_SIZE);
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
		*hash = asset_fnv64(d, (size_t)n, ASSET_FNV64_INIT);
	if (exe_size)
		*exe_size = (uint32_t)n;
	free(d);
	*status = 1;
	return mem;
}

uint64_t asset_fnv64(const void *p, size_t n, uint64_t h)
{
	const uint8_t *b = p;
	for (size_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 0x100000001b3ull;
	return h;
}

/* ---- the loaded image ---- */

static uint8_t *loaded;

static void install(uint8_t *mem)
{
	free(loaded);
	loaded = mem;
}

int asset_load_exe(const char *exe_path)
{
	int st;
	uint8_t *mem = asset_image_read(exe_path, &st, NULL, NULL);
	if (!mem)
		return st;
	install(mem);
	return 1;
}

const uint8_t *asset_image(void)
{
	return loaded;
}

void asset_release(void)
{
	free(loaded);
	loaded = NULL;
}

struct seg { uint32_t va, size, kind; };

/* Length of the zero run at a, up to end. */
static uint32_t zrun(const uint8_t *img, uint32_t a, uint32_t end)
{
	uint32_t z = a;
	while (z < end && img[z - IMAGE_BASE] == 0)
		z++;
	return z - a;
}

/* Split the kept ranges into data and zero runs (of MIN_ZERO or more) of
 * the image. */
static int split(const uint8_t *img, struct seg **out)
{
	size_t cap = 64, n = 0;
	struct seg *s = malloc(cap * sizeof *s);
	for (size_t k = 0; k < asset_content_n; k++) {
		uint32_t a = asset_content[k].lo, end = asset_content[k].hi;
		while (a < end) {
			uint32_t b = a, zl = zrun(img, a, end);
			int zero = zl >= MIN_ZERO;
			if (zero) {
				b = a + zl;
			} else {
				while (b < end) {
					if (img[b - IMAGE_BASE]) {
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

int asset_make(const char *exe_path, const char *out_path)
{
	int st;
	uint64_t exe_hash = 0;
	uint32_t exe_size = 0;
	uint8_t *img = asset_image_read(exe_path, &st, &exe_hash, &exe_size);
	if (!img)
		return st;
	struct seg *seg;
	int n = split(img, &seg);
	uint32_t payload = 0;
	for (int i = 0; i < n; i++) {
		/* the game code must never be in the asset */
		if (seg[i].va < GAME_CODE_END && seg[i].va + seg[i].size > GAME_CODE_START) {
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
			memcpy(pay, img + (seg[i].va - IMAGE_BASE), seg[i].size);
			pay += seg[i].size;
		}
	}
	memcpy(buf, "N2AS", 4);
	put32(buf + 4, ASSET_VERSION);
	put32(buf + 8, 0x39697c6au);
	put32(buf + 12, 0xc1000u);
	put32(buf + 16, exe_size);
	put32(buf + 20, (uint32_t)n);
	put64(buf + 24, exe_hash);
	put32(buf + 40, payload);
	put64(buf + 32, asset_fnv64(buf + HDR, total - HDR, ASSET_FNV64_INIT));
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

int asset_read(const char *path)
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
	int bad = total < HDR || memcmp(d, "N2AS", 4) || (get32(d + 4) < 1 || get32(d + 4) > ASSET_VERSION) ||
		  get32(d + 8) != 0x39697c6au || get32(d + 12) != 0xc1000u;
	uint32_t n = bad ? 0 : get32(d + 20), payload = bad ? 0 : get32(d + 40);
	if (!bad && (n > 4096 || HDR + 12ull * n + payload != total ||
		     asset_fnv64(d + HDR, total - HDR, ASSET_FNV64_INIT) != get64(d + 32)))
		bad = 1;
	uint8_t *mem = bad ? NULL : calloc(1, IMAGE_SIZE);
	if (!mem) {
		free(d);
		return bad ? -1 : 0;
	}
	const uint8_t *tab = d + HDR, *pay = tab + 12u * n;
	size_t left = payload;
	for (uint32_t i = 0; i < n && !bad; i++) {
		uint32_t va = get32(tab + 12 * i), size = get32(tab + 12 * i + 4), kind = get32(tab + 12 * i + 8);
		if (va < IMAGE_BASE || va - IMAGE_BASE > IMAGE_SIZE || size > IMAGE_SIZE - (va - IMAGE_BASE) || kind > 1 ||
		    (va < GAME_CODE_END && va + size > GAME_CODE_START) || (!kind && size > left)) {
			bad = 1;
			break;
		}
		if (!kind) {
			memcpy(mem + (va - IMAGE_BASE), pay, size);
			pay += size;
			left -= size;
		}
	}
	free(d);
	if (bad || left) {
		free(mem);
		return -1;
	}
	install(mem);
	return 1;
}
