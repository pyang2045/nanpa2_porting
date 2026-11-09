/*
 * Control flow of the C engine (R2, docs/refactor.md "R2: control flow").
 *
 * The game logic waits in places: it pumps messages, steps animations until a
 * frame has passed, waits for a click. In the C engine each wait is an ordinary
 * C loop that calls ctl_pump(); the platform layer runs inside the pump on the
 * same thread (events, timer callbacks, presentation), as the original's
 * PeekMessage loop did. A WM_QUIT makes the pump leave the engine with
 * ctl_quit(), which unwinds to the one place that called ctl_catch(): the top
 * of the game, or (while pieces of the register-level VM still exist) a
 * wrapper that runs direct-style code from a piece.
 *
 * Cost and polls are the pacing of the game (the clock follows the
 * instruction count, R0), kept exactly as the original code's; R3 replaces
 * them. COST(n) charges n instructions, POLL(at) is a backward jump of the
 * original at native address `at`: it runs the timer once enough
 * instructions have passed. Both are independent of the CPU model.
 */
#ifndef NANPA2_GAME_CTL_H
#define NANPA2_GAME_CTL_H

#include <stdint.h>

#include "x86/x86.h"

#define COST(n) X86_COUNT(n)
#define POLL(at) X86_POLL(at)

/* The message pump (native 0x401bf0): 0 on WM_QUIT, else 1. Charges what the
 * pump charges. Leaves ECX = EDX = 0 as the native pump did. */
int ctl_pump(void);

/* WM_QUIT seen: the 0x40638d reset (`mov esp, [top]`, COST(1)), then unwind
 * to the innermost ctl_catch, which returns 0. Not returning. The native
 * tail after the reset (pop ebp, ret to WinMain: 2 instructions) is the
 * catcher's to charge. */
_Noreturn void ctl_quit(void);

/* Pump; on WM_QUIT ctl_quit(). The usual wait loop step. */
void ctl_pump_or_quit(void);

/* Run fn(arg) with a quit catch: 1 if it returned, 0 if the game quit inside
 * (the stack of C calls is gone, the caller finishes the unwinding). Nests. */
int ctl_catch(void (*fn)(void *), void *arg);

/* Transitional, for the pieces of the register-level VM that remain: run
 * direct-style fn(arg) from a piece. Returns next_pc when fn returned, and
 * on WM_QUIT sets ESP to the VM's top frame and returns the pc 0x406393 where
 * the dispatcher finishes the quit (pop ebp, ret to WinMain). */
uint32_t ctl_piece(void (*fn)(void *), void *arg, uint32_t next_pc);

/* ---- the boundary to the HLE layer (R4 removes it) ----
 * Call the Win32 import, COM method, C runtime function or game unit at
 * native address `addr` as the code called it: the arguments are pushed on
 * the emulated stack (args[0] first argument), the callee runs, the stack is
 * restored (cdecl and stdcall alike), the result is EAX. No instruction cost
 * is charged: the caller charges its own pushes and call as before. */
uint32_t hle_invoke(uint32_t addr, int nargs, const uint32_t *args);
#define HLE(addr, ...) hle_invoke((addr), (int)(sizeof((uint32_t[]){ __VA_ARGS__ }) / 4), \
	(const uint32_t[]){ __VA_ARGS__ })
#define HLE0(addr) hle_invoke((addr), 0, NULL)

#endif
