/* File access in C (src/game/file.c) */
#ifndef NANPA2_GAME_FILE_H
#define NANPA2_GAME_FILE_H

#include <stddef.h>
#include <stdint.h>

/* 0x401050: load the game file `name` (looked up under DATA\, then under the
 * CD prefix; .GP4 and .ANI are decompressed) into dst. A file that cannot be
 * opened is skipped. `dst` has cap bytes (a file that is longer traps). The
 * files come from the platform (platform/files.h). */
void game_file_load(const char *name, void *dst, size_t cap);
/* The first cap bytes of an uncompressed file (the FLAG files, read by the save serializer,
 * game/state_view.h): the number of bytes read, 0 if it cannot be opened. */
size_t game_file_read(const char *name, void *dst, size_t cap);
/* 0x401160: write size bytes at buf to the file `name` ("wb", as given). The
 * FLAG save image is game_file_save(name, &G.save, SAVE_SIZE). */
void game_file_save(const char *name, const void *buf, uint32_t size);

/* The same by G address (game/gmem.h): the name is a string in G, the buffer lies in G */
void game_file_load_at(uint32_t name, uint32_t buf);
void game_file_save_at(uint32_t name, uint32_t buf, uint32_t size);

#endif
