/* Start-up, shutdown and the window procedure (src/game/startup.c) */
#ifndef NANPA2_GAME_STARTUP_H
#define NANPA2_GAME_STARTUP_H

#include <stdint.h>

/* WinMain (0x402090): runs the whole game; returns when the script VM ends */
uint32_t game_main(void);
/* The window procedure (0x4021c0), called by the message pump. */
uint32_t game_wndproc(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);

/* The 16 ms timer tick (0x4028c0, src/game/timer.c) */
void game_timer_tick(void);

#endif
