/* File access in C (src/game/file.c) */
#ifndef NANPA2_GAME_FILE_H
#define NANPA2_GAME_FILE_H

#include <stdint.h>

/* 0x401050: load the game file `name` (looked up under DATA\, then under the
 * CD prefix; .GP4 and .ANI are decompressed) into buf. A file that cannot be
 * opened is skipped. `name` and `buf` lie in the emulated address space (the
 * state object G, or the emulated heap and stack): the C runtime that reads
 * them is behind the HLE boundary. Registers are not part of the contract. */
void game_file_load(const char *name, void *buf);
/* 0x401160: write size bytes at buf to the file `name` ("wb", as given). The
 * FLAG save image is game_file_save(name, &G.save, SAVE_SIZE). */
void game_file_save(const char *name, const void *buf, uint32_t size);

/* The same by guest address, for the register-level pieces that still hold
 * addresses */
void game_file_load_at(uint32_t name, uint32_t buf);
void game_file_save_at(uint32_t name, uint32_t buf, uint32_t size);

#endif
