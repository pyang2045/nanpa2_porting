/* The 64-bit hash of the replay checkpoints, 8 bytes a step, multiply and rotate. Neutral so the
 * game's state hash (game/state_view.c) and the dev layer (dev/replay.c) share it. */
#ifndef NANPA2_HASH_H
#define NANPA2_HASH_H

#include <stddef.h>
#include <stdint.h>

uint64_t hash_bytes(const void *p, size_t n, uint64_t seed);

#endif
