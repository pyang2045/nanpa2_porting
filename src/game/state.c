#include "game/state.h"

struct game_state G;

void state_init(void)
{
	x86_alias(DATA_BASE, &G, sizeof G);
}
