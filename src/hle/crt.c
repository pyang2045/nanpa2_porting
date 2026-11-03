/* C runtime functions called by the lifted code, by native address. */
#include "hle.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fsys.h"

#define MAX_FILES 32
#define FILE_HANDLE_BASE 0x00010000u /* fake FILE* values; never dereferenced */

static FILE *files[MAX_FILES];
static char save_root[1024];

void crt_init(const char *game_dir, const char *save_dir)
{
	fsys_set_root(game_dir);
	snprintf(save_root, sizeof save_root, "%s", save_dir);
}

static int is_save_name(const char *p)
{
	return !strncmp(p, "FLAG", 4) || !strncmp(p, "flag", 4);
}

/* Native paths: "Q:\DATA\x" (the CD, see GetDriveTypeA), "DATA\x" or
 * "FLAG0" relative to the install folder. The CD and install folder are both
 * the game folder here, except that FLAG files live in the save folder. */
static FILE *open_native(const char *path, const char *mode)
{
	const char *p = path;
	if (((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') && p[1] == ':')
		p += 2;
	while (*p == '\\' || *p == '/')
		p++;
	int writing = strchr(mode, 'w') != NULL;
	if (is_save_name(p)) {
		char full[1200];
		snprintf(full, sizeof full, "%s/%s", save_root, p);
		FILE *f = fopen(full, writing ? "wb" : "rb");
		if (f || writing)
			return f;
		return fsys_open(p); /* initial FLAG files from data1.cab */
	}
	if (writing)
		return NULL;
	return fsys_open(p);
}

static uint32_t file_handle(FILE *f)
{
	for (int i = 0; i < MAX_FILES; i++)
		if (!files[i]) {
			files[i] = f;
			return FILE_HANDLE_BASE + 0x20u * (uint32_t)i;
		}
	fclose(f);
	return 0;
}

static FILE *file_get(uint32_t h)
{
	uint32_t i = (h - FILE_HANDLE_BASE) / 0x20u;
	if (h < FILE_HANDLE_BASE || i >= MAX_FILES)
		return NULL;
	return files[i];
}

/* sprintf with arguments on the emulated stack (first vararg at argp). */
static uint32_t do_sprintf(uint32_t dst, uint32_t fmt, uint32_t argp)
{
	char out[1024];
	size_t n = 0;
	for (uint32_t f = fmt;; f++) {
		uint32_t c = rd8(f);
		if (!c)
			break;
		if (c != '%') {
			if (n + 1 < sizeof out)
				out[n++] = (char)c;
			continue;
		}
		char spec[16];
		size_t sl = 0;
		spec[sl++] = '%';
		for (f++; (c = rd8(f)) && strchr("-+ #0123456789.lh", (int)c) && sl < 12; f++)
			spec[sl++] = (char)c;
		if (!c)
			break;
		spec[sl++] = (char)c;
		spec[sl] = 0;
		char tmp[512];
		switch (c) {
		case 's':
			hle_strcpy_in(tmp, rd32(argp), sizeof tmp);
			argp += 4;
			{
				char t2[600];
				snprintf(t2, sizeof t2, spec, tmp);
				snprintf(tmp, sizeof tmp, "%s", t2);
			}
			break;
		case 'd': case 'i':
			snprintf(tmp, sizeof tmp, spec, (int)rd32(argp));
			argp += 4;
			break;
		case 'u': case 'x': case 'X': case 'o': case 'c':
			snprintf(tmp, sizeof tmp, spec, rd32(argp));
			argp += 4;
			break;
		case '%':
			tmp[0] = '%';
			tmp[1] = 0;
			break;
		default:
			x86_trap(0, "sprintf: unsupported conversion");
		}
		for (char *t = tmp; *t && n + 1 < sizeof out; t++)
			out[n++] = *t;
	}
	out[n] = 0;
	memcpy(x86_ptr(dst, (uint32_t)n + 1), out, n + 1);
	return (uint32_t)n;
}

static int ci_cmp(uint32_t a, uint32_t b, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		int ca = tolower((int)rd8(a + i)), cb = tolower((int)rd8(b + i));
		if (ca != cb)
			return ca - cb;
		if (!ca)
			return 0;
	}
	return 0;
}

int crt_call(uint32_t addr)
{
	switch (addr) {
	case 0x412c90: /* sprintf(buf, fmt, ...) */
		cpu.r[EAX] = do_sprintf(hle_arg(0), hle_arg(1), cpu.r[ESP] + 8);
		return 1;
	case 0x412d13: { /* fopen(name, mode) */
		char name[512], mode[8];
		hle_strcpy_in(name, hle_arg(0), sizeof name);
		hle_strcpy_in(mode, hle_arg(1), sizeof mode);
		FILE *f = open_native(name, mode);
		if (getenv("NANPA2_LOG"))
			fprintf(stderr, "fopen %s %s -> %s\n", name, mode, f ? "ok" : "fail");
		cpu.r[EAX] = f ? file_handle(f) : 0;
		return 1;
	}
	case 0x412d6e: { /* fclose(fp) */
		FILE *f = file_get(hle_arg(0));
		if (f) {
			fclose(f);
			files[(hle_arg(0) - FILE_HANDLE_BASE) / 0x20u] = NULL;
		}
		cpu.r[EAX] = 0;
		return 1;
	}
	case 0x412deb: { /* fread(buf, size, n, fp) */
		FILE *f = file_get(hle_arg(3));
		uint32_t total = hle_arg(1) * hle_arg(2);
		size_t got = f && total ? fread(x86_ptr(hle_arg(0), total), 1, total, f) : 0;
		cpu.r[EAX] = hle_arg(1) ? (uint32_t)got / hle_arg(1) : 0;
		return 1;
	}
	case 0x412fb7: { /* fwrite(buf, size, n, fp) */
		FILE *f = file_get(hle_arg(3));
		uint32_t total = hle_arg(1) * hle_arg(2);
		size_t put = f && total ? fwrite(x86_ptr(hle_arg(0), total), 1, total, f) : 0;
		cpu.r[EAX] = hle_arg(1) ? (uint32_t)put / hle_arg(1) : 0;
		return 1;
	}
	case 0x41a730: /* _fileno(fp): our handle index */
		cpu.r[EAX] = (hle_arg(0) - FILE_HANDLE_BASE) / 0x20u;
		return 1;
	case 0x41a6a6: { /* _filelength(fd) */
		uint32_t i = hle_arg(0);
		FILE *f = i < MAX_FILES ? files[i] : NULL;
		long len = -1;
		if (f) {
			long cur = ftell(f);
			fseek(f, 0, SEEK_END);
			len = ftell(f);
			fseek(f, cur, SEEK_SET);
		}
		cpu.r[EAX] = (uint32_t)len;
		return 1;
	}
	case 0x412f02: /* malloc(n) */
		cpu.r[EAX] = x86_malloc(hle_arg(0));
		return 1;
	case 0x412d26: /* free(p) */
		x86_free(hle_arg(0));
		return 1;
	case 0x412f90: { /* strrchr(s, c) */
		uint32_t s = hle_arg(0), c = hle_arg(1) & 0xff, last = 0;
		for (;; s++) {
			uint32_t ch = rd8(s);
			if (ch == c)
				last = s;
			if (!ch)
				break;
		}
		cpu.r[EAX] = last;
		return 1;
	}
	case 0x41a3d0: /* _stricmp(a, b) */
		cpu.r[EAX] = (uint32_t)ci_cmp(hle_arg(0), hle_arg(1), 0xffffffffu);
		return 1;
	case 0x41a740: /* _strnicmp(a, b, n) */
		cpu.r[EAX] = (uint32_t)ci_cmp(hle_arg(0), hle_arg(1), hle_arg(2));
		return 1;
	}
	return 0;
}
