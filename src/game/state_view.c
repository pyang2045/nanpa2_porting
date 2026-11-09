#include "game/state_view.h"

#include <string.h>

#include "game/state.h"
#include "game/vm.h"

void state_save_image(uint8_t out[STATE_SAVE_SIZE])
{
	memcpy(out, &G.save, STATE_SAVE_SIZE);
}

uint32_t state_frame(void)
{
	return rd32(0x446292);
}
