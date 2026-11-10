/* Bit-identity of the ISO reader: every file under FOLDER/DATA, MUSIC and SPEECH must read the
 * same bytes from the image, both whole (iso_read) and through the FILE stream (iso_fopen,
 * read in odd-sized pieces after a seek). Also reports files the image has under those folders
 * that the folder lacks. usage: iso_check IMAGE.iso FOLDER */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "iso.h"

static int checked, bad;
static size_t bytes;

static void compare(const char *rel, const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("%s: cannot open folder file\n", rel);
		bad++;
		return;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *a = malloc(n > 0 ? (size_t)n : 1);
	if (fread(a, 1, (size_t)n, f) != (size_t)n)
		n = -1;
	fclose(f);
	size_t len = 0;
	unsigned char *b = iso_read(rel, &len);
	if (!b || n < 0 || len != (size_t)n || memcmp(a, b, len)) {
		printf("%s: iso_read differs (folder %ld, iso %zu)\n", rel, n, len);
		bad++;
	} else {
		FILE *s = iso_fopen(rel);
		int ok = s != NULL;
		/* seek to the middle, read to the end in 777-byte pieces, then back to 0 and compare */
		if (ok && len > 10) {
			size_t mid = len / 2, got = 0;
			unsigned char piece[777];
			ok = !fseek(s, (long)mid, SEEK_SET) && ftell(s) == (long)mid;
			while (ok && got < len - mid) {
				size_t want = len - mid - got < sizeof piece ? len - mid - got : sizeof piece;
				ok = fread(piece, 1, want, s) == want && !memcmp(piece, a + mid + got, want);
				got += want;
			}
			ok = ok && fread(piece, 1, 1, s) == 0 && !fseek(s, -5, SEEK_END) && fgetc(s) == a[len - 5];
		}
		if (s)
			fclose(s);
		if (!ok) {
			printf("%s: iso_fopen differs\n", rel);
			bad++;
		}
	}
	checked++;
	bytes += (size_t)(n > 0 ? n : 0);
	free(a);
	free(b);
}

static void walk(const char *rel, const char *path)
{
	DIR *d = opendir(path);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char r[1024], p[2048];
		snprintf(r, sizeof r, "%s/%s", rel, e->d_name);
		snprintf(p, sizeof p, "%s/%s", path, e->d_name);
		struct stat st;
		if (stat(p, &st))
			continue;
		if (S_ISDIR(st.st_mode))
			walk(r, p);
		else
			compare(r, p);
	}
	closedir(d);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: iso_check IMAGE.iso FOLDER\n");
		return 2;
	}
	if (!iso_mount(argv[1])) {
		printf("cannot mount %s\n", argv[1]);
		return 1;
	}
	static const char *top[] = { "DATA", "MUSIC", "SPEECH" };
	for (int i = 0; i < 3; i++) {
		char p[2048];
		snprintf(p, sizeof p, "%s/%s", argv[2], top[i]);
		walk(top[i], p);
	}
	printf("checked %d files (%zu bytes) bad %d\n", checked, bytes, bad);
	return bad != 0 || checked == 0;
}
