/* docs.h on iOS/iPadOS: the app's Documents folder, from SDL. */
#include <SDL3/SDL.h>
#include <string.h>

#include "docs.h"

const char *docs_dir(void)
{
	static char path[1024];
	if (!path[0]) {
		const char *d = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS); /* owned by SDL, ends in a slash */
		if (d && strlen(d) < sizeof path) {
			strcpy(path, d);
			size_t n = strlen(path);
			while (n > 1 && path[n - 1] == '/')
				path[--n] = 0;
		}
	}
	return path[0] ? path : NULL;
}
