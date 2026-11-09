/*
 * Deterministic mode, recording and replay (docs/refactor.md, R0).
 *
 * The game's clock is the 16 ms timer, and its input comes from the host.
 * Both make two runs of the same play differ. In deterministic mode
 * (NANPA2_DETERMINISTIC=1, and implied by --record and --replay) the game
 * clock is the virtual time of the pacing model (src/game/pace.h,
 * src/hle/clock.h; whatever --speed says), the message pump takes the host event queue at a
 * count of pumps and not of milliseconds, and nothing the host or the audio
 * thread does reaches the game's state. Two runs with the same input are
 * then identical to the bit.
 *
 * A recording (--record FILE, NANPA2_RECORD) is a text file: the messages
 * that reached the game from outside (host mouse and keys, NANPA2_INPUT,
 * the exploration bot), each stamped with the game tick (timer callbacks
 * run so far) and the number of the pump within that tick, and a
 * checkpoint every N ticks (NANPA2_CP_TICKS, default 250) with hashes of the
 * game state:
 *   li.*   layout independent: the screen, the palette, the saved variables
 *          (the 0xc00-byte image the save file holds), the scripts reached,
 *          the FLAG files in the save folder. They survive the refactor.
 *   raw.*  the emulated memory (.data and .bss in pages of 4 KB, heap, stack)
 *          and the virtual clock; for checking the same code twice.
 * --replay FILE posts the recorded messages at the same pump and compares
 * every checkpoint with the recorded one; the first difference stops the
 * run (exit code 3) and names the checkpoint and what differs.
 * NANPA2_REPLAY_CHECK=li compares only the li.* keys.
 */
#ifndef NANPA2_REPLAY_H
#define NANPA2_REPLAY_H

#include <stddef.h>
#include <stdint.h>

int replay_det(void);        /* deterministic clock */
int replay_replaying(void);  /* host input, scripts and the bot are off */
/* After hle_init: open the recording or replay file named by the
 * environment (NANPA2_RECORD, NANPA2_REPLAY); save_dir is the FLAG folder. */
void replay_init(const char *save_dir);
/* Virtual milliseconds: instructions run / 25000 */
uint64_t replay_ms(void);
/* In win_pump, after the timer ran and before host events are taken:
 * counts pumps, posts due recorded messages, takes due checkpoints. */
void replay_pump(void);
/* A message from outside the game (host, script, bot): recorded. Returns 0
 * when it must be dropped (replaying: only the recording posts). */
int replay_external(uint32_t msg, uint32_t wparam, uint32_t lparam, int from_replay);
/* A game data file was opened (name as the game gave it); a VM opcode fetch */
void replay_note_file(const char *name);
void replay_note_vm(uint32_t off);
/* At exit: the final checkpoint, the file closed */
void replay_finish(void);

/* Hashing, shared with tools of the same kind */
uint64_t replay_hash(const void *p, size_t n, uint64_t seed);

#endif
