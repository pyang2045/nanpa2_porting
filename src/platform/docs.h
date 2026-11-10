/* The app's user-visible documents folder (iOS/iPadOS: <app>/Documents, shared through Finder's file
 * sharing and the Files app, where the player puts the game folder). docs_ios.m; elsewhere there is
 * no such folder and the function returns NULL. The returned path lives for the whole run. */
#ifndef NANPA2_PLATFORM_DOCS_H
#define NANPA2_PLATFORM_DOCS_H

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
const char *docs_dir(void);
#else
static inline const char *docs_dir(void)
{
	return (const char *)0;
}
#endif

#endif
