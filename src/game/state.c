#include "game/state.h"

#include <string.h>

#include "asset.h"

struct game_state G;

int state_init(void)
{
	const uint8_t *image = asset_image();
	if (!image)
		return 0;
	/* the data area of the exe as the loader would lay it out: the initialised data, the rest zero */
	memcpy(&G, image + (DATA_BASE - IMAGE_BASE), sizeof G);
	asset_release();
	return 1;
}
