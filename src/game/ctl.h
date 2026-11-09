/*
 * Control flow of the C engine (R2, docs/refactor.md "R2: control flow").
 *
 * The game logic waits in places: it pumps messages, steps animations until a
 * frame has passed, waits for a click. In the C engine each wait is an ordinary
 * C loop that calls ctl_pump(); the platform layer runs inside the pump on the
 * same thread (events, timer callbacks, presentation), as the original's
 * PeekMessage loop did. A WM_QUIT makes the pump leave the engine with
 * ctl_quit(), which unwinds to the one place that called ctl_catch(): the top
 * of the game.
 *
 * Pacing (R3): the game waits on the 16 ms timer and spends time only in the
 * few kinds of busy work of pace.h, each an explicit ctl_work_us(). There are
 * no per-instruction charges.
 */
#ifndef NANPA2_GAME_CTL_H
#define NANPA2_GAME_CTL_H

#include <stdint.h>

#include "game/gmem.h"

/* Busy work of the original machine: advance the game clock by `us` microseconds (src/game/pace.h
 * lists the costs and where they come from). */
void ctl_work_us(uint32_t us);

/* The message pump (native 0x401bf0): 0 on WM_QUIT, else 1. Costs PACE_PUMP_US. */
int ctl_pump(void);

/* NANPA2_TIMELOG=1: one stderr line "T <game ms> <frame> <label> <arg>" per probe: fade, wipe,
 * text and walk events, file opens. The game ms is the deterministic clock, else the host clock. */
void ctl_tlog(const char *label, uint32_t arg);

/* WM_QUIT seen: unwind to the innermost ctl_catch, which returns 0. Not returning. */
_Noreturn void ctl_quit(void);

/* Pump; on WM_QUIT ctl_quit(). The usual wait loop step. */
void ctl_pump_or_quit(void);

/* Run fn(arg) with a quit catch: 1 if it returned, 0 if the game quit inside
 * (the stack of C calls is gone, the caller finishes the unwinding). Nests. */
int ctl_catch(void (*fn)(void *), void *arg);

#endif
