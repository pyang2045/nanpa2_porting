/*
 * File access: opening a game file 0x401000, loading a whole file 0x401050 and saving one
 * 0x401160.
 *
 * A name is looked up under DATA\ (unless it starts with "FLAG", the save slots), first as given and
 * then under the CD prefix the start-up search found (0x431030). A file whose name ends in .GP4
 * or .ANI is read whole into a buffer and decompressed into the caller's (src/lzh.c); anything
 * else is read straight into it. A file that cannot be opened is silently skipped. The files
 * come from the platform layer (src/platform/files.h), which keeps the original's paths.
 * Semantics: re-notes.md "File access".
 *
 * The destination is part of G (a script segment, a picture buffer, the save image), given as a
 * G address and bounded by the end of G: natively the loader wrote wherever the address went,
 * here a file that would run past the end of G traps.
 */
#include "game/file.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/ctl.h"
#include "game/pace.h"
#include "game/state.h"
#include "lzh.h"
#include "platform/files.h"
#include "x86/x86.h"

#define CD_PREFIX 0x431030u   /* string: where the CD search found the game's CD */

static int starts_with_ci(const char *s, const char *prefix)
{
	for (; *prefix; s++, prefix++)
		if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix) || !*s)
			return 0;
	return 1;
}

static int ends_with_ci(const char *s, const char *ext)
{
	size_t n = strlen(s), e = strlen(ext);
	if (n < e)
		return 0;
	for (size_t i = 0; i < e; i++)
		if (tolower((unsigned char)s[n - e + i]) != tolower((unsigned char)ext[i]))
			return 0;
	return 1;
}

/* 0x401000: fopen(name, "rb"); if that fails, fopen(CD prefix + name). */
static FILE *open_game_file(const char *name)
{
	FILE *f = files_open(name, "rb");
	if (!f) {
		char path[512];
		snprintf(path, sizeof path, "%s%s", (const char *)x86_ptr(CD_PREFIX, 1), name);
		f = files_open(path, "rb");
	}
	return f;
}

/* 0x401050: load the file `name` into dst (cap bytes are there), see above. */
void game_file_load(const char *name, void *dst, size_t cap)
{
	char path[512];
	if (starts_with_ci(name, "FLAG")) /* the save slots: as given, not under DATA\ */
		snprintf(path, sizeof path, "%s", name);
	else
		snprintf(path, sizeof path, "DATA\\%s", name);
	FILE *f = open_game_file(path);
	if (!f)
		return;
	long size = files_size(f);
	if (size < 0)
		size = 0;
	const char *dot = strrchr(name, '.');
	int packed = dot && (ends_with_ci(dot, ".GP4") || ends_with_ci(dot, ".ANI"));
	if (!packed) {
		if ((size_t)size > cap)
			x86_trap(0x401050, "a file runs past the end of the game state");
		size_t got = fread(dst, 1, (size_t)size, f);
		(void)got;
		fclose(f);
		return;
	}
	/* The native loader read the file into a heap block whose size, rounded up to 16 with an
	 * 8-byte header, the decompressor took as the input's length: the file's bytes and zeros.
	 * The pacing model charges for that length. */
	size_t block = ((size_t)size + 8 + 15) & ~(size_t)15, in_len = block - 8;
	unsigned char *img = calloc(1, in_len);
	if (!img)
		x86_trap(0x401050, "out of memory");
	size_t got = fread(img, 1, (size_t)size, f);
	(void)got;
	fclose(f);
	size_t n;
	unsigned char *out = lzh_decode(img, in_len, &n);
	free(img);
	if (!out)
		x86_trap(0x4075f0, "corrupt GP4/ANI data");
	if (n > cap)
		x86_trap(0x4075f0, "a picture runs past the end of the game state");
	memcpy(dst, out, n);
	ctl_work_us(PACE_DECODE_US(in_len, n));
	free(out);
}

/* 0x401160: write `size` bytes at `buf` to the file `name` ("wb"; the file name is used as
 * given, no DATA\ prefix). */
void game_file_save(const char *name, const void *buf, uint32_t size)
{
	FILE *f = files_open(name, "wb");
	if (!f)
		return;
	size_t put = fwrite(buf, 1, size, f);
	(void)put;
	fclose(f);
}

void game_file_load_at(uint32_t name, uint32_t buf)
{
	game_file_load((const char *)x86_ptr(name, 1), x86_ptr(buf, 1), DATA_END - buf);
}

void game_file_save_at(uint32_t name, uint32_t buf, uint32_t size)
{
	game_file_save((const char *)x86_ptr(name, 1), x86_ptr(buf, size), size);
}
