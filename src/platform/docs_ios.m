/* docs.h on iOS/iPadOS: the app's Documents folder. */
#import <Foundation/Foundation.h>

#include <string.h>

#include "docs.h"

const char *docs_dir(void)
{
	static char path[1024];
	if (!path[0]) {
		NSArray *dirs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
		if (dirs.count)
			strlcpy(path, [dirs[0] fileSystemRepresentation], sizeof path);
	}
	return path[0] ? path : NULL;
}
