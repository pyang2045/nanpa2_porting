/* dialog.h without a native toolkit (Linux, Windows for now): no picker. */
#include "dialog.h"

int dialog_pick_folder(const char *prompt, char *out, size_t cap)
{
	(void)prompt;
	(void)out;
	(void)cap;
	return 0;
}
