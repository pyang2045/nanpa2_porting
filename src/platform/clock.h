/* The game clock and its pacing (R3, docs/refactor.md "R3: pacing").
 *
 * The original ran on a 486-class PC: its busy work (script expressions, text drawing, picture
 * decoding, the dissolve) took time, and between that work it waited on the 16 ms multimedia
 * timer. The port keeps a virtual machine time in microseconds, advanced by the explicit costs of
 * src/game/pace.h and by one cost per message pump. The timer callback that is the game's clock
 * runs against one of two sources:
 *
 *   real time x speed        the default: the game runs at the original's speed (speed 1.0), and
 *                            the work costs are made real by sleeping when the virtual time is
 *                            ahead of the host's
 *   the virtual time         deterministic mode (recording, replaying, NANPA2_DETERMINISTIC) and
 *                            speed 0: as fast as the host can, and repeatable, because nothing
 *                            reads a host clock
 */
#ifndef NANPA2_CLOCK_H
#define NANPA2_CLOCK_H

#include <stdint.h>

/* Speed 1.0 is the original's, 2.0 twice as fast; 0 = as fast as the host can, on the virtual clock. */
void clock_init(double speed);
double clock_speed(void);

/* virtual machine time since the start, in microseconds */
uint64_t clock_us(void);

/* advance the virtual time (no service) */
void clock_add(uint32_t us);

/* busy work of the original machine: advance, then keep the host up (throttle to real time, run the
 * timer callback if due, present now and then). The only way game code spends time. */
void clock_work(uint32_t us);

/* the game timer's clock in ms (deterministic or speed 0: the virtual time) */
uint64_t clock_timer_ms(void);

/* milliseconds for the timing log (NANPA2_TIMELOG): the virtual time in deterministic mode, else the host's */
uint64_t clock_log_ms(void);

/* a monotonic host clock in nanoseconds (for budgets and timings, not game time) */
uint64_t clock_host_ns(void);

void clock_lat(const char *what); /* NANPA2_LATLOG: "L <us> <what>" to stderr */

/* offline mode (platform/offline.h): back to time 0; jump the virtual time forward to a game ms */
void clock_reset(void);
void clock_skip_to_ms(uint64_t ms);

/* sleep if the virtual time is ahead of real time x speed */
void clock_throttle(void);

#endif
