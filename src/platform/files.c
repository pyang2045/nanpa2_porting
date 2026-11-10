/* Platform API: files. See files.h. */
#include "platform/files.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "fsys.h"
#include "hooks.h"

static char save_root[1024];

void files_init(const char *game_dir, const char *save_dir)
{
	fsys_set_root(game_dir);
	snprintf(save_root, sizeof save_root, "%s", save_dir);
}

static int is_save_name(const char *p)
{
	return !strncmp(p, "FLAG", 4) || !strncmp(p, "flag", 4);
}

/* "Q:\DATA\x" (the CD), "DATA\x" or "FLAG0": relative to the install folder. The CD and the
 * install folder are both the game folder here, except that FLAG files live in the save
 * folder. */
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
		int n = snprintf(full, sizeof full, "%s/", save_root);
		for (const char *q = p; *q && n > 0 && n < (int)sizeof full - 1; q++)
			full[n++] = (char)tolower((unsigned char)*q);
		if (n > 0 && n < (int)sizeof full)
			full[n] = 0;
		FILE *f = fopen(full, writing ? "wb" : "rb");
		if (f || writing)
			return f;
		return fsys_open(p); /* the FLAG files of a copied install; none: the game generates a blank */
	}
	if (writing)
		return NULL;
	return fsys_open(p);
}

FILE *files_open(const char *path, const char *mode)
{
	FILE *f = open_native(path, mode);
	hook_file_opened(path);
	if (getenv("NANPA2_LOG"))
		fprintf(stderr, "fopen %s %s -> %s\n", path, mode, f ? "ok" : "fail");
	return f;
}

long files_size(FILE *f)
{
	long cur = ftell(f);
	if (cur < 0 || fseek(f, 0, SEEK_END))
		return -1;
	long len = ftell(f);
	fseek(f, cur, SEEK_SET);
	return len;
}

int files_drive_is_cd(char drive)
{
	return (drive | 0x20) == 'q';
}
