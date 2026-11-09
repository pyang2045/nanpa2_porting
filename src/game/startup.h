/* Start-up, shutdown and the window procedure (src/game/startup.c) */
#ifndef NANPA2_GAME_STARTUP_H
#define NANPA2_GAME_STARTUP_H

#include <stdint.h>

/* WinMain (0x402090): runs the whole game; returns when the script VM ends */
uint32_t game_main(uint32_t hinstance);
/* The window procedure (0x4021c0), called by the message pump. The caller
 * keeps the CPU registers across it. */
uint32_t game_wndproc(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);

#endif
