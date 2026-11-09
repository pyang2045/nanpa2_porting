#include "game/state_view.h"

#include <string.h>

#include "game/vm.h"

void state_save_image(uint8_t out[STATE_SAVE_SIZE])
{
	memcpy(out, x86_ptr(VM_SCRIPT_NAME, STATE_SAVE_SIZE), STATE_SAVE_SIZE);
}

uint32_t state_frame(void)
{
	return rd32(0x446292);
}
