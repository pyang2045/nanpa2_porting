#include "game/state.h"

struct game_state G;

void state_init(void)
{
	x86_alias(SAVE_BASE, &G.save, sizeof G.save);
	x86_alias(RT_BASE, &G.rt, sizeof G.rt);
	x86_alias(SC_BASE, &G.sc, sizeof G.sc);
	x86_alias(AN_BASE, &G.an, sizeof G.an);
}
