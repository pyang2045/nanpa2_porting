#ifndef NANPA2_FSYS_H
#define NANPA2_FSYS_H

#include <stddef.h>
#include <stdio.h>

/* Set the game folder (the CD contents), or a .iso image of the CD (iso.h): every call
 * below then reads from the image. Saves are never written there. */
void fsys_set_root(const char *dir);
const char *fsys_root(void);

int fsys_is_iso(void);
/* The files directly in dir of an ISO root (original case); 0 if the root is a folder. */
int fsys_list_iso(const char *dir, void (*cb)(const char *name, void *arg), void *arg);

/* Open a game file by its DOS-style relative path ("DATA\\01.GP4").
 * Backslashes become slashes and each component is matched
 * case-insensitively, since the CD mixes "MUSIC/00.mp3" with "DATA/01.GP4". */
FILE *fsys_open(const char *rel);

/* The host path of a game file (matched like fsys_open); 1 if it exists. A
 * path is written either way (the file to create). */
int fsys_path(const char *rel, char *out, size_t cap);

/* Read a whole game file into a malloc'd buffer. */
unsigned char *fsys_read(const char *rel, size_t *len);

/* Read a file the way native 0x401050 does: "DATA\\name", decompressed
 * when the extension is .GP4 or .ANI. */
unsigned char *fsys_load_data(const char *name, size_t *len);

#endif
