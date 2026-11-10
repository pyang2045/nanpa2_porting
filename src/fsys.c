#include "fsys.h"
#include "iso.h"
#include "lzh.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <dirent.h>
#include <strings.h>
#endif

static char root[1024] = ".";

void fsys_set_root(const char *dir)
{
	snprintf(root, sizeof root, "%s", dir);
	size_t n = strlen(root);
	while (n > 1 && (root[n - 1] == '/' || root[n - 1] == '\\'))
		root[--n] = 0;
	/* a regular file is the CD image (iso.h); a folder is the extracted CD */
	struct stat st;
	if (!stat(root, &st) && S_ISREG(st.st_mode)) {
		if (!iso_mount(root))
			iso_unmount();
	} else
		iso_unmount();
}

int fsys_is_iso(void)
{
	return iso_mounted();
}

int fsys_list_iso(const char *dir, void (*cb)(const char *name, void *arg), void *arg)
{
	if (!iso_mounted())
		return 0;
	iso_list(dir, cb, arg);
	return 1;
}

const char *fsys_root(void)
{
	return root;
}

#ifdef _WIN32
static int resolve(const char *rel, char *out, size_t cap)
{
	snprintf(out, cap, "%s\\%s", root, rel);
	for (char *p = out; *p; p++)
		if (*p == '/')
			*p = '\\';
	return 1;
}
#else
/* Append one path component, matching it case-insensitively. */
static int append_component(char *path, size_t cap, const char *comp)
{
	size_t n = strlen(path);
	snprintf(path + n, cap - n, "/%s", comp);
	FILE *f = fopen(path, "rb");
	if (f) {
		fclose(f);
		return 1;
	}
	path[n] = 0;
	DIR *d = opendir(path);
	if (!d) {
		snprintf(path + n, cap - n, "/%s", comp);
		return 0;
	}
	struct dirent *e;
	int found = 0;
	while ((e = readdir(d))) {
		if (!strcasecmp(e->d_name, comp)) {
			snprintf(path + n, cap - n, "/%s", e->d_name);
			found = 1;
			break;
		}
	}
	closedir(d);
	if (!found)
		snprintf(path + n, cap - n, "/%s", comp);
	return found;
}

static int resolve(const char *rel, char *out, size_t cap)
{
	char tmp[512];
	snprintf(tmp, sizeof tmp, "%s", rel);
	snprintf(out, cap, "%s", root);
	int ok = 1;
	for (char *tok = strtok(tmp, "\\/"); tok; tok = strtok(NULL, "\\/"))
		ok = append_component(out, cap, tok);
	return ok;
}
#endif

int fsys_path(const char *rel, char *out, size_t cap)
{
	if (iso_mounted()) {
		snprintf(out, cap, "%s:%s", iso_image_path(), rel);
		return iso_stat(rel, NULL, NULL);
	}
	return resolve(rel, out, cap);
}

FILE *fsys_open(const char *rel)
{
	if (iso_mounted())
		return iso_fopen(rel);
	char path[1024];
	resolve(rel, path, sizeof path);
	return fopen(path, "rb");
}

unsigned char *fsys_read(const char *rel, size_t *len)
{
	if (iso_mounted())
		return iso_read(rel, len);
	FILE *f = fsys_open(rel);
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *buf = malloc(n > 0 ? (size_t)n : 1);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	if (buf && len)
		*len = (size_t)n;
	return buf;
}

static int has_ext(const char *name, const char *ext)
{
	const char *dot = strrchr(name, '.');
#ifdef _WIN32
	return dot && !_stricmp(dot, ext);
#else
	return dot && !strcasecmp(dot, ext);
#endif
}

unsigned char *fsys_load_data(const char *name, size_t *len)
{
	char rel[256];
	snprintf(rel, sizeof rel, "DATA\\%s", name);
	size_t n;
	unsigned char *raw = fsys_read(rel, &n);
	if (!raw)
		return NULL;
	if (!has_ext(name, ".GP4") && !has_ext(name, ".ANI")) {
		if (len)
			*len = n;
		return raw;
	}
	unsigned char *out = lzh_decode(raw, n, len);
	free(raw);
	return out;
}
