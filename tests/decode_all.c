/* Decode every .GP4/.ANI under GAME/DATA; check GP4 sizes match the header.
 * usage: decode_all GAME   (GAME: the game folder or a .iso image of the CD) */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "fsys.h"

static char **names;
static int nnames;

static void add_name(const char *name, void *arg)
{
	(void)arg;
	names = realloc(names, sizeof *names * (nnames + 1));
	names[nnames++] = strdup(name);
}

int main(int argc, char **argv)
{
	char path[1024];
	fsys_set_root(argc > 1 ? argv[1] : "game");
	snprintf(path, sizeof path, "%s/DATA", fsys_root());
	if (!fsys_list_iso("DATA", add_name, NULL)) {
		DIR *d = opendir(path);
		if (!d)
			return 1;
		struct dirent *de;
		while ((de = readdir(d)))
			add_name(de->d_name, NULL);
		closedir(d);
	}
	int ok = 0, bad = 0;
	for (int i = 0; i < nnames; i++) {
		struct { const char *d_name; } ent = { names[i] }, *e = &ent;
		const char *dot = strrchr(e->d_name, '.');
		if (!dot || (strcasecmp(dot, ".GP4") && strcasecmp(dot, ".ANI")))
			continue;
		size_t n;
		unsigned char *b = fsys_load_data(e->d_name, &n);
		int good = b != NULL;
		if (good && !strcasecmp(dot, ".GP4")) {
			unsigned w = b[4] | b[5] << 8, h = b[6] | b[7] << 8;
			good = n == 520 + (size_t)w * h;
			if (!good)
				printf("%s: %zu bytes, header %ux%u\n", e->d_name, n, w, h);
		}
		if (!b)
			printf("%s: decode failed\n", e->d_name);
		good ? ok++ : bad++;
		free(b);
	}
	printf("ok %d bad %d\n", ok, bad);
	return bad != 0;
}
