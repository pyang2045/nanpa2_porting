/*
 * The built-in data of the exe (R4, docs/refactor.md "The asset").
 *
 * The game's initialised data (tables, strings, the start values of the globals) lives in the
 * player's nanpa2.exe, which this engine never runs: nanpa2.dat is cut from it once (src/asset.c,
 * format in re-notes.md "nanpa2.dat") and holds only data ranges, no game code. Loading it
 * (or the exe, for the dev modes) produces an image laid out as the exe's loader would: the byte
 * at virtual address a is image[a - IMAGE_BASE]. The game state `G` is initialised from the
 * defaults in code (state_defaults) and from the few ranges of game text in the image
 * (asset_content); the image is released after that.
 */
#ifndef NANPA2_ASSET_H
#define NANPA2_ASSET_H

#include <stddef.h>
#include <stdint.h>

#define IMAGE_BASE 0x00400000u
#define IMAGE_SIZE 0x000c1000u
/* The game code of the exe: it is never part of the asset */
#define GAME_CODE_START 0x00401000u
#define GAME_CODE_END 0x00407800u

/* The ranges of the image that G cannot be given by code: the game's own text, kept in the asset
 * (state_init copies exactly these; an older asset that holds more is read just the same) */
struct asset_range { uint32_t lo, hi; };
extern const struct asset_range asset_content[];
extern const unsigned asset_content_n;

/* 1 if the first n bytes of a file are the header of that nanpa2.exe (the Taiwan release built
 * 2000-07-10: the native addresses throughout the engine are this build's) */
int asset_exe_check(const uint8_t *d, size_t n);
/* The exe image without installing it: a fresh IMAGE_SIZE bytes, or NULL with *status 0 (cannot
 * read) or -1 (not this nanpa2.exe). hash and exe_size may be NULL. */
uint8_t *asset_image_read(const char *exe_path, int *status, uint64_t *hash, uint32_t *exe_size);
uint64_t asset_fnv64(const void *p, size_t n, uint64_t h);
#define ASSET_FNV64_INIT 0xcbf29ce484222325ull

/* nanpa2.dat: asset_make cuts it from the exe (1 written, 0 io error, -1 exe is not this build);
 * asset_read loads it (1 loaded, 0 cannot read, -1 not an asset of this format and build);
 * asset_load_exe loads the exe's image whole instead (1, 0, -1 as above). */
int asset_make(const char *exe_path, const char *out_path);
int asset_read(const char *path);
int asset_load_exe(const char *exe_path);

/* The loaded image (NULL before a load) and its release */
const uint8_t *asset_image(void);
void asset_release(void);

#endif
