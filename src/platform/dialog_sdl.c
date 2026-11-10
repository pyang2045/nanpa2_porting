/* Folder picker (dialog.h) for the platforms without a native one here (Windows, Linux): SDL's
 * SDL_ShowOpenFolderDialog. It is asynchronous, so events are pumped until its callback runs. It picks a
 * folder (the CD's contents); a .iso image is still found by the folder scans and --game. macOS uses
 * dialog_macos.m, whose panel takes a folder or a file in one go. iOS has no such dialog (the call
 * fails and returns 0; launch.c never asks there). Untested outside macOS. */
#include "dialog.h"

#include <SDL3/SDL.h>
#include <stdio.h>

struct pick {
	SDL_AtomicInt done;
	char path[4096];
	int ok;
};

static void SDLCALL picked(void *ud, const char *const *list, int filter)
{
	struct pick *p = ud;
	(void)filter;
	if (list && list[0] && snprintf(p->path, sizeof p->path, "%s", list[0]) < (int)sizeof p->path)
		p->ok = 1;
	SDL_SetAtomicInt(&p->done, 1);
}

#ifndef SDL_PLATFORM_IOS
int dialog_saves_popup(void)
{
	return 0; /* iOS only for now (dialog_ios.m) */
}
#endif

int dialog_pick_folder(const char *prompt, char *out, size_t cap)
{
	(void)prompt; /* SDL's dialog has no message line */
	static struct pick p;
	p.ok = 0;
	SDL_SetAtomicInt(&p.done, 0);
	SDL_ShowOpenFolderDialog(picked, &p, NULL, NULL, false);
	while (!SDL_GetAtomicInt(&p.done)) {
		SDL_PumpEvents();
		SDL_Delay(20);
	}
	return p.ok && snprintf(out, cap, "%s", p.path) < (int)cap;
}
