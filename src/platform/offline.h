/*
 * Platform API: the offline mode.
 *
 * The game can be run to produce something (the start save, dev/startsave.c) without a session:
 * nothing is shown or played, host input is ignored (only a quit request gets through), no logo,
 * and a pump that only waits for the next 16 ms timer tick jumps the virtual clock to it instead of
 * spinning (the game's own work still costs what pace.h says). It needs the deterministic clock
 * (the dev layer's hooks say so) and speed 0. Normal play and replays never switch it on, so
 * their behaviour is byte for byte what it was.
 *
 * offline_end resets what a run leaves in the platform (surfaces and palette, window queue and
 * timer, font handles, the virtual clock) so that the real session starts clean.
 */
#ifndef NANPA2_PLATFORM_OFFLINE_H
#define NANPA2_PLATFORM_OFFLINE_H

void offline_begin(void);
void offline_end(void);
int offline_on(void);
/* The pumps in a row that find no timer tick due before the next one is jumped to. The game's script
 * loop pumps once per opcode and polls for input by running opcodes, so an idle wait is hundreds of
 * pumps (21 us each) in a 16 ms tick; but a loop that pumps a fixed number of times must keep its
 * length, so only a long run is skipped. */
#define OFFLINE_IDLE_PUMPS 16
/* idle pumps jump the virtual clock to the next timer tick (on by default in offline mode; NANPA2_START_SAVE_NOFF turns it off to measure) */
int offline_fast_forward(void);
void offline_set_fast_forward(int on);

#endif
