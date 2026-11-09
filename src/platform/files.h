/*
 * Platform API: files (R4, docs/refactor.md "The platform API").
 *
 * The game opens its files by the original's DOS paths: "DATA\x" relative to the install
 * folder, the same behind the CD prefix ("Q:\DATA\x"), and "FLAGn" for the save slots. This
 * layer maps them to the host: the game folder (each component matched case-insensitively,
 * src/fsys.c), and the FLAG files in the save folder (lower case there, whatever case the game
 * asks for, so a case-sensitive file system finds what was written; a FLAG file the save
 * folder does not have is looked up in the game folder, where the install put the initial ones).
 * Every open is noted for the dev log and the replay checkpoints, failures included.
 */
#ifndef NANPA2_PLATFORM_FILES_H
#define NANPA2_PLATFORM_FILES_H

#include <stdio.h>

/* the game folder (the CD contents) and the folder the FLAG files live in */
void files_init(const char *game_dir, const char *save_dir);

/* fopen with the game's path rules; NULL if it cannot be opened (or a write is asked of a
 * game file: only the FLAG files can be written) */
FILE *files_open(const char *path, const char *mode);

/* the size of an open file in bytes, or -1 */
long files_size(FILE *f);

/* whether the CD-ROM drive letter `drive` ('A'..'Z') holds the game: the game folder is the CD
 * in drive Q (the native GetDriveTypeA answer) */
int files_drive_is_cd(char drive);

#endif
