/* Where the engine gets the exe's built-in data from: nanpa2.dat (src/asset.c),
 * cut once from the player's nanpa2.exe, or the exe itself for the modes that
 * run its code. */
#ifndef NANPA2_ASSETS_H
#define NANPA2_ASSETS_H

#include <stddef.h>

/* NULL if the game folder can run: a valid nanpa2.dat in it or in the save
 * folder (launch_pref_dir), or a nanpa2.exe of the right build to make one
 * from. Otherwise the reason. Needs fsys_set_root(dir). */
const char *assets_check(const char *dir);

/* Put the built-in data into the image (asset_read, asset_load_exe) that state_init copies the game state from, from the
 * game folder (fsys root). exe_arg is --exe PATH or NULL.
 *   - with --exe, in the recomp build and in the dev modes that run the
 *     original code (NANPA2_DIFF, _DIFF_DIR, _FUZZ, _LOCKSTEP, _INTERP,
 *     _START_ORACLE) the exe is loaded whole; it must exist
 *   - otherwise nanpa2.dat from the game folder, then the save folder
 *   - no usable asset and an exe: the asset is made (game folder, or the
 *     save folder if that is not writable) and loaded; if it cannot be
 *     written anywhere the exe is loaded whole
 * 1 ok; 0 failed, with the reason in err. */
int assets_open(const char *exe_arg, char *err, size_t cap);

/* nanpa2 --make-assets: cut nanpa2.dat from the exe (exe_arg or the game
 * folder's) into out, or by default into the game folder / the save
 * folder. 1 ok, 0 failed (reason in err); path receives the file written. */
int assets_make(const char *exe_arg, const char *out, char *path, size_t pathcap, char *err, size_t cap);

#endif
