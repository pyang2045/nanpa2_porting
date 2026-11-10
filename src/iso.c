/* Read-only ISO 9660 reader (iso.h). The index is built once at mount: the volume has
 * a few thousand files, so every directory is walked and the entries kept sorted by their
 * normalized path (lower case, "/" separators). Files are single contiguous extents, so a
 * read is one pread (or seek + fread) on the image. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "iso.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define SECTOR 2048

struct entry {
	char *key;  /* normalized full path */
	char *disp; /* last component, original case */
	int64_t off;
	uint32_t size;
	int dir;
};

static char image[1024];
static int mounted;
static struct entry *ents;
static size_t nents, capents;
#ifdef _WIN32
static FILE *img;
#else
static int img = -1;
#endif

static int img_read(int64_t off, void *buf, size_t n)
{
#ifdef _WIN32
	if (!img || _fseeki64(img, off, SEEK_SET))
		return 0;
	return fread(buf, 1, n, img) == n;
#else
	unsigned char *p = buf;
	while (n) {
		ssize_t r = pread(img, p, n, (off_t)off);
		if (r <= 0)
			return 0;
		p += r;
		off += r;
		n -= (size_t)r;
	}
	return 1;
#endif
}

static uint32_t le32(const unsigned char *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void add(const char *key, const char *disp, int64_t off, uint32_t size, int dir)
{
	if (nents == capents) {
		capents = capents ? capents * 2 : 1024;
		ents = realloc(ents, capents * sizeof *ents);
		if (!ents)
			abort();
	}
	ents[nents].key = strdup(key);
	ents[nents].disp = strdup(disp);
	ents[nents].off = off;
	ents[nents].size = size;
	ents[nents].dir = dir;
	nents++;
}

static int cmp(const void *a, const void *b)
{
	return strcmp(((const struct entry *)a)->key, ((const struct entry *)b)->key);
}

/* Record name -> UTF-8 without the ";1" version; Joliet names are UCS-2 big-endian. */
static void record_name(const unsigned char *id, int len, int joliet, char *out, size_t cap)
{
	size_t n = 0;
	if (joliet) {
		for (int i = 0; i + 1 < len && n + 4 < cap; i += 2) {
			unsigned c = id[i] << 8 | id[i + 1];
			if (c < 0x80)
				out[n++] = (char)c;
			else if (c < 0x800) {
				out[n++] = (char)(0xc0 | c >> 6);
				out[n++] = (char)(0x80 | (c & 0x3f));
			} else {
				out[n++] = (char)(0xe0 | c >> 12);
				out[n++] = (char)(0x80 | ((c >> 6) & 0x3f));
				out[n++] = (char)(0x80 | (c & 0x3f));
			}
		}
	} else {
		for (int i = 0; i < len && n + 1 < cap; i++)
			out[n++] = (char)id[i];
	}
	out[n] = 0;
	char *semi = strchr(out, ';');
	if (semi)
		*semi = 0;
	n = strlen(out);
	if (!joliet && n > 1 && out[n - 1] == '.') /* "NAME." of a name without extension */
		out[n - 1] = 0;
}

static void walk(int64_t extent, uint32_t length, const char *prefix, int joliet, int depth)
{
	if (depth > 16 || length > (64u << 20))
		return;
	unsigned char *buf = malloc(length ? length : 1);
	if (!buf || !img_read(extent * SECTOR, buf, length)) {
		free(buf);
		return;
	}
	for (uint32_t p = 0; p < length;) {
		unsigned len = buf[p];
		if (!len) { /* records do not cross sectors */
			p = (p / SECTOR + 1) * SECTOR;
			continue;
		}
		if (p + len > length || len < 34)
			break;
		const unsigned char *r = buf + p;
		p += len;
		int idlen = r[32];
		if (33 + idlen > (int)len || (idlen == 1 && r[33] <= 1))
			continue; /* "." and ".." */
		if (r[25] & 0x80) {
			fprintf(stderr, "iso: multi-extent file not supported, skipped\n");
			continue;
		}
		char disp[512], key[1100];
		record_name(r + 33, idlen, joliet, disp, sizeof disp);
		int dir = (r[25] & 2) != 0;
		snprintf(key, sizeof key, "%s%s", prefix, disp);
		for (char *c = key; *c; c++)
			*c = (char)tolower((unsigned char)*c);
		int64_t ext = (int64_t)le32(r + 2) + r[1]; /* extent + extended attribute length */
		uint32_t size = le32(r + 10);
		add(key, disp, ext * SECTOR, size, dir);
		if (dir) {
			char sub[1100];
			snprintf(sub, sizeof sub, "%s/", key);
			walk(ext, size, sub, joliet, depth + 1);
		}
	}
	free(buf);
}

void iso_unmount(void)
{
	for (size_t i = 0; i < nents; i++) {
		free(ents[i].key);
		free(ents[i].disp);
	}
	free(ents);
	ents = NULL;
	nents = capents = 0;
#ifdef _WIN32
	if (img)
		fclose(img);
	img = NULL;
#else
	if (img >= 0)
		close(img);
	img = -1;
#endif
	mounted = 0;
	image[0] = 0;
}

int iso_mount(const char *path)
{
	if (mounted && !strcmp(path, image))
		return 1;
	iso_unmount();
#ifdef _WIN32
	img = fopen(path, "rb");
	int ok = img != NULL;
#else
	img = open(path, O_RDONLY);
	int ok = img >= 0;
#endif
	if (!ok)
		return 0;
	unsigned char vd[SECTOR];
	int64_t pvd = -1, svd = -1;
	for (int s = 16; s < 64; s++) {
		if (!img_read((int64_t)s * SECTOR, vd, SECTOR) || memcmp(vd + 1, "CD001", 5))
			break;
		if (vd[0] == 255)
			break;
		if (vd[0] == 1 && pvd < 0)
			pvd = s;
		/* Joliet: a supplementary descriptor with escape sequence %/@, %/C or %/E */
		if (vd[0] == 2 && svd < 0 && vd[88] == '%' && vd[89] == '/' &&
		    (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E'))
			svd = s;
	}
	if (pvd < 0) {
		iso_unmount();
		return 0;
	}
	int joliet = svd >= 0;
	img_read((joliet ? svd : pvd) * SECTOR, vd, SECTOR);
	const unsigned char *root = vd + 156;
	walk(le32(root + 2), le32(root + 10), "", joliet, 0);
	qsort(ents, nents, sizeof *ents, cmp);
	snprintf(image, sizeof image, "%s", path);
	mounted = 1;
	return 1;
}

int iso_mounted(void)
{
	return mounted;
}

const char *iso_image_path(void)
{
	return image;
}

/* "DATA\\01.GP4", "/data//01.gp4" -> "data/01.gp4" */
static void normalize(const char *rel, char *out, size_t cap)
{
	size_t n = 0;
	int sep = 1;
	for (; *rel && n + 1 < cap; rel++) {
		if (*rel == '/' || *rel == '\\') {
			if (!sep)
				out[n++] = '/';
			sep = 1;
		} else {
			out[n++] = (char)tolower((unsigned char)*rel);
			sep = 0;
		}
	}
	if (n && out[n - 1] == '/')
		n--;
	out[n] = 0;
}

static const struct entry *find(const char *rel)
{
	char k[1100];
	normalize(rel, k, sizeof k);
	struct entry key = { .key = k };
	return bsearch(&key, ents, nents, sizeof *ents, cmp);
}

int iso_stat(const char *rel, int64_t *off, uint32_t *size)
{
	const struct entry *e = mounted ? find(rel) : NULL;
	if (!e || e->dir)
		return 0;
	if (off)
		*off = e->off;
	if (size)
		*size = e->size;
	return 1;
}

unsigned char *iso_read(const char *rel, size_t *len)
{
	int64_t off;
	uint32_t size;
	if (!iso_stat(rel, &off, &size))
		return NULL;
	unsigned char *buf = malloc(size ? size : 1);
	if (buf && !img_read(off, buf, size)) {
		free(buf);
		return NULL;
	}
	if (buf && len)
		*len = size;
	return buf;
}

void iso_list(const char *dir, void (*cb)(const char *, void *), void *arg)
{
	char k[1100];
	normalize(dir, k, sizeof k);
	size_t kl = strlen(k);
	if (kl)
		k[kl++] = '/';
	for (size_t i = 0; mounted && i < nents; i++)
		if (!ents[i].dir && !strncmp(ents[i].key, k, kl) && !strchr(ents[i].key + kl, '/'))
			cb(ents[i].disp, arg);
}

#ifdef _WIN32
/* No fmemopen or cookie streams: a temporary file deleted on close. */
FILE *iso_fopen(const char *rel)
{
	size_t n;
	unsigned char *d = iso_read(rel, &n);
	if (!d)
		return NULL;
	char dir[MAX_PATH], name[MAX_PATH];
	FILE *f = NULL;
	if (GetTempPathA(sizeof dir, dir) && GetTempFileNameA(dir, "n2i", 0, name)) {
		f = fopen(name, "w+bD");
		if (f && (fwrite(d, 1, n, f) != n || fseek(f, 0, SEEK_SET))) {
			fclose(f);
			f = NULL;
		}
	}
	free(d);
	return f;
}
#else
struct cookie {
	int64_t base, pos;
	uint32_t size;
};

static int64_t ck_read(struct cookie *c, char *buf, size_t n)
{
	if (c->pos >= (int64_t)c->size)
		return 0;
	if (n > c->size - (uint64_t)c->pos)
		n = c->size - (uint64_t)c->pos;
	if (!img_read(c->base + c->pos, buf, n))
		return -1;
	c->pos += (int64_t)n;
	return (int64_t)n;
}

static int64_t ck_seek(struct cookie *c, int64_t off, int whence)
{
	int64_t np = whence == SEEK_SET ? off : whence == SEEK_CUR ? c->pos + off : (int64_t)c->size + off;
	if (np < 0)
		return -1;
	c->pos = np;
	return np;
}

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
static int f_read(void *c, char *b, int n)
{
	return (int)ck_read(c, b, (size_t)n);
}
static fpos_t f_seek(void *c, fpos_t off, int w)
{
	return ck_seek(c, off, w);
}
static int f_close(void *c)
{
	free(c);
	return 0;
}
#define OPEN_COOKIE(c) funopen(c, f_read, NULL, f_seek, f_close)
#else /* glibc, musl, bionic */
static ssize_t f_read(void *c, char *b, size_t n)
{
	return ck_read(c, b, n);
}
static int f_seek(void *c, off64_t *off, int w)
{
	int64_t r = ck_seek(c, *off, w);
	if (r < 0)
		return -1;
	*off = r;
	return 0;
}
static int f_close(void *c)
{
	free(c);
	return 0;
}
#define OPEN_COOKIE(c) fopencookie(c, "rb", (cookie_io_functions_t){ f_read, NULL, f_seek, f_close })
#endif

FILE *iso_fopen(const char *rel)
{
	struct cookie *c = calloc(1, sizeof *c);
	if (!c)
		return NULL;
	if (!iso_stat(rel, &c->base, &c->size)) {
		free(c);
		return NULL;
	}
	FILE *f = OPEN_COOKIE(c);
	if (!f)
		free(c);
	return f;
}
#endif
