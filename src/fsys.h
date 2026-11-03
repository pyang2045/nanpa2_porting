#ifndef NANPA2_FSYS_H
#define NANPA2_FSYS_H

#include <stddef.h>
#include <stdio.h>

/* Set the game folder (the CD contents). */
void fsys_set_root(const char *dir);
const char *fsys_root(void);

/* Open a game file by its DOS-style relative path ("DATA\\01.GP4").
 * Backslashes become slashes and each component is matched
 * case-insensitively, since the CD mixes "MUSIC/00.mp3" with "DATA/01.GP4". */
FILE *fsys_open(const char *rel);

/* Read a whole game file into a malloc'd buffer. */
unsigned char *fsys_read(const char *rel, size_t *len);

/* Read a file the way native 0x401050 does: "DATA\\name", decompressed
 * when the extension is .GP4 or .ANI. */
unsigned char *fsys_load_data(const char *name, size_t *len);

#endif
