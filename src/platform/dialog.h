/* Native dialogs that SDL does not have. macOS: dialog_macos.m (AppKit);
 * elsewhere dialog_none.c (no picker: the game folder comes from --game,
 * NANPA2_GAME_DIR, the remembered path or the folder next to the
 * executable). Call from the main thread after SDL_Init(SDL_INIT_VIDEO), so
 * SDL owns the application object. */
#ifndef NANPA2_DIALOG_H
#define NANPA2_DIALOG_H

#include <stddef.h>

/* Ask for a folder. prompt: the panel's message (UTF-8). Writes the chosen
 * path (UTF-8) to out; returns 1, or 0 when cancelled or unavailable. */
int dialog_pick_folder(const char *prompt, char *out, size_t cap);

#endif
