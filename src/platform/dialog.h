/* Folder picker. macOS: dialog_macos.m (AppKit: SDL's dialogs take a folder or a file, not
 * either in one panel, and a .iso is as good as a folder); elsewhere dialog_sdl.c
 * (SDL_ShowOpenFolderDialog; none on iOS: the game folder comes from Documents, --game,
 * NANPA2_GAME_DIR, the remembered path or the folder next to the executable). Call from the main thread after SDL_Init(SDL_INIT_VIDEO), so
 * SDL owns the application object. */
#ifndef NANPA2_DIALOG_H
#define NANPA2_DIALOG_H

#include <stddef.h>

/* Ask for a folder. prompt: the panel's message (UTF-8). Writes the chosen
 * path (UTF-8) to out; returns 1, or 0 when cancelled or unavailable. */
int dialog_pick_folder(const char *prompt, char *out, size_t cap);

/* The saves popup (iOS only, dialog_ios.m; elsewhere it returns 0 at once): the save folder holds
 * saves, so offer to remove them and start over with a name of the player's own. Continue (the
 * default, also after 5 s) returns 0; Remove saves asks once more (no timeout) and returns 1 only
 * when the player confirms Delete. Main thread, after the window exists. */
int dialog_saves_popup(void);

#endif
