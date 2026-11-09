#include "hle.h"

#include "platform/files.h"

/* Set the host layer up: the folders the files come from (platform/files.h) */
int hle_init(const char *game_dir, const char *save_dir)
{
	files_init(game_dir, save_dir);
	return 1;
}
