/*
 * The start save: a game that begins after the Prologue.
 *
 * The title offers the Prologue, the main story ("Main") and Data Load, and the game offers Main
 * only when FLAG6 exists, which the game writes itself when the Prologue ends (OPEN1.MES, the
 * save op at 0x2185, slot 6, guarded by flag 2033). A fresh install has no save, so the player
 * would have to sit through the Prologue first. startsave_make plays it for them, offline: it runs the
 * game's own scripts with the exploration bot under fixed rules (explore_policy_begin: the name
 * 林大偉 entered on the name grid, the first or least-tried item of every menu, no
 * randomness), on the deterministic clock, with nothing shown or played and idle waits skipped
 * (platform/offline.h), and stops at the first FLAG6 the game writes after OPEN1.MES opened.
 * That file is the result: FLAG6, and the same image as FLAG0 (Data Load then lights diary 1).
 * Nothing of the game's data or of a recorded save is in the program; the save is produced here by
 * the game.
 *
 * The files are produced in a staging folder inside the save folder and moved into place only if
 * neither exists then; an existing save is never touched.
 */
#ifndef NANPA2_DEV_STARTSAVE_H
#define NANPA2_DEV_STARTSAVE_H

/* The save folder holds no FLAG file and (check_game_folder) the game folder's own FLAG files,
 * if any, are all the same blank. */
int startsave_wanted(const char *save_dir, int check_game_folder);

/* A save of the player's is there (FLAG0..6), and the player has not declined the start save. */
int startsave_saves_offered(const char *save_dir);

/* The player chose to start over: delete FLAG0..6 in the save folder (nothing else: not the game
 * folder, not display.txt) and write the marker startsave-off, which keeps the start save from coming
 * back. Delete the marker to bring it back. */
void startsave_decline(const char *save_dir);

/* Generate. show: draw a "preparing" bar when it takes over 2 s. Returns 1 written, 0 failed
 * (nothing written), -1 stopped by a quit request (nothing written). Call after files_init and
 * text_init; the game state is reset on return. */
int startsave_make(const char *save_dir, int show);

#endif
