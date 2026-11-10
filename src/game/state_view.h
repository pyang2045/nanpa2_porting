/*
 * The game state as the replay checkpoints see it (src/dev/replay.c): the
 * layout independent view. Only this file knows where the state lives, so
 * moving the state (docs/refactor.md, R1 and R4) changes
 * these functions and not the checkpoints.
 */
#ifndef NANPA2_GAME_STATE_VIEW_H
#define NANPA2_GAME_STATE_VIEW_H

#include <stddef.h>
#include <stdint.h>

/* The FLAG file: the one place where the game state is converted to and from the original's file
 * format (docs/refactor.md "The FLAG serializer"). The file is G.save, 0xc00 bytes at offset 0 of
 * the save image: the running script's name (32 bytes), the 4-bit flags (0x400 bytes), the 27
 * registers '@'..'Z' (u16), and the word array @[0..] with the named words of state.h
 * (`save_image`), all little-endian. The only field that holds an address is the four script
 * segment bases (G.save.n.segment, u32 at 0x44533c): addresses in the numbering of the
 * original's data area, which is how G indexes itself (game/gmem.h); the file holds them as the
 * original wrote them, and a load does not take them from the file: the VM set-up that follows
 * every load (vm_init) derives them again. Every other field is a value.
 *
 * state_save_export writes the image of the running state into out; state_save_import takes an
 * image from a file (n bytes read, possibly fewer than STATE_SAVE_SIZE: the rest of the state is
 * kept) into the state, segment bases excepted. */
#define STATE_SAVE_SIZE 0xc00u
void state_save_export(uint8_t out[STATE_SAVE_SIZE]);
/* The image of a new game's start-up state: what an unused FLAG file holds. */
void state_save_blank(uint8_t out[STATE_SAVE_SIZE]);
void state_save_import(const uint8_t *in, size_t n);
/* Hash of the whole of G (the data area, 0x431000-0x4c0000) with the fields that hold values of
 * the platform (handles, pixel pointers, the DirectDraw lock descriptor) zeroed first, so that
 * the hash depends on the game's state and not on how the platform names its objects (R4,
 * docs/refactor.md "The G hash"). */
uint64_t state_data_hash(void);
/* The game's frame counter */
uint32_t state_frame(void);

#endif
