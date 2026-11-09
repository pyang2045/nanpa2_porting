/*
 * The game state as the replay checkpoints see it (src/hle/replay.c): the
 * layout independent view. Only this file knows where the state lives, so
 * moving the state out of the emulated memory (docs/refactor.md, R1) changes
 * these functions and not the checkpoints.
 */
#ifndef NANPA2_GAME_STATE_VIEW_H
#define NANPA2_GAME_STATE_VIEW_H

#include <stdint.h>

/* The image the save file holds: script name, flags, registers, work words */
#define STATE_SAVE_SIZE 0xc00u
void state_save_image(uint8_t out[STATE_SAVE_SIZE]);
/* Hash of the whole of G (the data area, 0x431000-0x4c0000) with the fields that hold values of
 * the platform (handles, pixel pointers, the DirectDraw lock descriptor) zeroed first, so that
 * the hash depends on the game's state and not on how the platform names its objects (R4,
 * docs/refactor.md "The G hash"). */
uint64_t state_data_hash(void);
/* The game's frame counter */
uint32_t state_frame(void);

#endif
