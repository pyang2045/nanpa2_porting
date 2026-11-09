/*
 * Platform API: the window, the message queue, the timer and the mouse cursor (R4,
 * docs/refactor.md "The platform API").
 *
 * The game was written for Windows and its scripts depend on the way Windows delivers input, so
 * the platform keeps WM_*-style messages: the host's mouse, keyboard and focus events (and the
 * recorded or scripted ones of the dev layer) become messages {msg, wparam, lparam} in a queue,
 * one of which the pump hands to the game's window procedure each round. The numbers are
 * Windows': WM_MOUSEMOVE 0x200 (lparam = y << 16 | x in screen pixels), WM_LBUTTONDOWN 0x201
 * and up 0x202, WM_RBUTTONDOWN 0x204 and up 0x205, WM_KEYDOWN 0x100 and up 0x101 (wparam a
 * virtual key code), WM_ACTIVATEAPP 0x1c, WM_DESTROY 2, WM_QUIT 0x12 (the game ends).
 *
 * The game's 16 ms clock is a timer the platform runs against the game clock (src/hle/clock.h):
 * the tick callback runs from the pump and from the game's waits, on the game's thread, never
 * concurrently with it.
 */
#ifndef NANPA2_PLATFORM_WINDOW_H
#define NANPA2_PLATFORM_WINDOW_H

#include <stdint.h>

#define WM_DESTROY 0x0002
#define WM_QUIT 0x0012
#define WM_ACTIVATEAPP 0x001c
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205

struct win_handlers {
	/* the game's window procedure: one message of the queue */
	uint32_t (*wndproc)(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);
	/* the 16 ms timer tick */
	void (*timer)(void);
};

/* Open the (full-screen, 640x480) window: from now on the pump calls the handlers. Returns the
 * window handle the window procedure is given. */
uint32_t win_open(const struct win_handlers *h);
/* Show it: the game is activated (WM_ACTIVATEAPP 1 is queued). */
void win_show(void);

/* Queue a message from the game. WM_QUIT makes the pump return 0. */
void win_post(uint32_t msg, uint32_t wparam, uint32_t lparam);

/* One round of the message loop: services the host (status file, the game clock, the timer,
 * the presenter, SDL events), then takes one message from the queue and gives it to the window
 * procedure. 0 when the game must end (WM_QUIT), else 1. */
int win_pump(void);

/* The 16 ms game timer: starts at the game clock now, runs the tick callback for every 16 ms
 * the game clock has passed (a few at most per call) */
void win_timer_start(void);
void win_timer_stop(void);

/* The system cursor: a display counter (the native ShowCursor: shown while >= 0); returns the new
 * count. The host cursor is hidden only over the game image of the focused window. */
int win_show_cursor(int show);
/* Move the host pointer to the game's screen coordinates (640x480) */
void win_warp_cursor(int x, int y);

/* An error for the player (the original's MessageBox): the platform shows it or prints it */
void win_message_box(const char *text, const char *title);

#endif
