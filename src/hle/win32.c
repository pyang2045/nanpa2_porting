/* Platform API: the window, the message queue, the 16 ms timer and the cursor (platform/window.h). */
#include "platform/window.h"
#include "hle.h"
#include "game/ctl.h"
#include "game/pace.h"
#include "clock.h"
#include "replay.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev.h"
#include "status.h"
#include "platform/present.h"
#include "game/startup.h"
#include "game/state.h"

#define HWND_MAIN 0x00001000u

static struct win_handlers handlers;
static int timer_on;
static uint64_t timer_next; /* game ms (clock_timer_ms) */
/* the native ShowCursor display counter (0 at start; shown while >= 0) */
static int cursor_count;
static int cursor_shown = -1, cursor_over; /* host cursor state last set */

/* ---- message queue ---- */

#define QUEUE_LEN 256
static struct msg {
	uint32_t msg, wparam, lparam;
} queue[QUEUE_LEN];
static int q_head, q_count;

void win32_post_message(uint32_t msg, uint32_t wparam, uint32_t lparam)
{
	/* coalesce mouse moves, as Windows does */
	if (msg == WM_MOUSEMOVE && q_count) {
		struct msg *last = &queue[(q_head + q_count - 1) % QUEUE_LEN];
		if (last->msg == WM_MOUSEMOVE) {
			last->wparam = wparam;
			last->lparam = lparam;
			return;
		}
	}
	if (q_count == QUEUE_LEN)
		return;
	queue[(q_head + q_count) % QUEUE_LEN] = (struct msg){ msg, wparam, lparam };
	q_count++;
}

void win32_post_ext(uint32_t msg, uint32_t wparam, uint32_t lparam)
{
	if (replay_external(msg, wparam, lparam, 0))
		win32_post_message(msg, wparam, lparam);
}

static uint32_t sdl_to_vk(SDL_Keycode k)
{
	switch (k) {
	case SDLK_RETURN: case SDLK_KP_ENTER: return 0x0d;
	case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
	case SDLK_ESCAPE: return 0x1b;
	case SDLK_SPACE: return 0x20;
	case SDLK_END: return 0x23;
	case SDLK_LEFT: return 0x25;
	case SDLK_UP: return 0x26;
	case SDLK_RIGHT: return 0x27;
	case SDLK_DOWN: return 0x28;
	case SDLK_INSERT: return 0x2d;
	case SDLK_F1: return 0x70;
	}
	if (k >= SDLK_a && k <= SDLK_z)
		return 'A' + (uint32_t)(k - SDLK_a);
	if (k >= SDLK_0 && k <= SDLK_9)
		return '0' + (uint32_t)(k - SDLK_0);
	return 0;
}

/* Native cursor use (USER32 IAT slot 0x41b11c, ShowCursor):
 *   0x401e27 ShowCursor(0)  first thing in the logo player 0x401e20 (also
 *                           when no movie plays), never undone: count -1
 *   0x40262a ShowCursor(1)  start of DirectDraw init 0x402620: 0
 *   0x4028ab ShowCursor(0)  end of 0x402620: -1 for the whole game
 *   0x402ac6 ShowCursor(1)  shutdown 0x402a20: 0
 * The window class (0x402090) has LoadCursorA(0, IDC_ARROW) at 0x4020cd;
 * the window procedure 0x4021c0 has no WM_SETCURSOR, and there is no
 * SetCursor or SetClassLong import. In the game the system cursor is thus
 * hidden over the exclusive full-screen DirectDraw display, and the game
 * draws its own cursor sprite (DATA/MOUSE.DAT, blinked by the timer
 * 0x4028c0 through 0x4ac510/0x4ac514) at the WM_MOUSEMOVE position.
 * The port hides the host cursor only over the game image: letterbox
 * borders, the title bar, other windows and an unfocused window keep the
 * host cursor. */
void win32_cursor_update(void)
{
	SDL_Window *w = SDL_GetMouseFocus();
	int over = 0;
	if (w && (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS)) {
		int wx, wy, sx, sy;
		SDL_GetMouseState(&wx, &wy);
		over = present_window_to_screen(wx, wy, &sx, &sy);
	}
	cursor_over = over;
	int shown = cursor_count >= 0 || !over;
	if (shown != cursor_shown) {
		cursor_shown = shown;
		SDL_ShowCursor(shown ? SDL_ENABLE : SDL_DISABLE);
	}
}

int win_show_cursor(int show)
{
	cursor_count += show ? 1 : -1;
	win32_cursor_update();
	return cursor_count;
}

void win32_cursor_state(int *count, int *shown, int *over_image)
{
	*count = cursor_count;
	*shown = SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE;
	*over_image = cursor_over;
}

static uint32_t mouse_lparam(int wx, int wy)
{
	int sx, sy;
	present_window_to_screen(wx, wy, &sx, &sy);
	return ((uint32_t)sy << 16) | (uint32_t)sx;
}

/* WM_ACTIVATEAPP(0) clears 0x4ac4ec, which stops the 16 ms timer (0x4028c0)
 * and the frame counter the game waits on, and pauses music (0x403220): the
 * game freezes, on a black screen if a fade was running. The port keeps
 * running in the background unless --pause-inactive (NANPA2_PAUSE_INACTIVE). */
static int pause_inactive(void)
{
	static int on = -1;
	if (on < 0)
		on = getenv("NANPA2_PAUSE_INACTIVE") && atoi(getenv("NANPA2_PAUSE_INACTIVE"));
	return on;
}

/* Move SDL events into the native queue. */
static void collect_events(void)
{
	SDL_Event ev;
	/* deterministic: the scripts and the bot run at every pump, and the host
	 * queue every 256th (messages carry the pump they came at) */
	static unsigned n;
	if (replay_det()) {
		if (!replay_replaying())
			dev_inject();
		if (n++ & 255)
			return;
	} else {
		dev_inject();
	}
	/* scripted runs only take SDL_QUIT from the host, so the Cocoa event
	 * pump (most of their host CPU) can run every 16 ms */
	if (dev_scripted() && !replay_det()) {
		static uint32_t last;
		uint32_t now = SDL_GetTicks();
		if (now - last < 16)
			return;
		last = now;
	}
	int cursor_check = 0;
	while (SDL_PollEvent(&ev)) {
		if (ev.type == SDL_MOUSEMOTION || ev.type == SDL_WINDOWEVENT)
			cursor_check = 1;
		/* scripted runs: the real cursor, keys and focus changes on a shared
		 * desktop would move the injected mouse or stop the timer
		 * (WM_ACTIVATEAPP 0 clears 0x4ac4ec) */
		if (dev_scripted() && ev.type != SDL_QUIT)
			continue;
		switch (ev.type) {
		case SDL_QUIT:
			win32_post_ext(WM_QUIT, 0, 0);
			break;
		case SDL_MOUSEMOTION:
			win32_post_ext(WM_MOUSEMOVE, 0, mouse_lparam(ev.motion.x, ev.motion.y));
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: {
			int down = ev.type == SDL_MOUSEBUTTONDOWN;
			uint32_t lp = mouse_lparam(ev.button.x, ev.button.y);
			if (ev.button.button == SDL_BUTTON_LEFT)
				win32_post_ext(down ? WM_LBUTTONDOWN : WM_LBUTTONUP, 0, lp);
			else if (ev.button.button == SDL_BUTTON_RIGHT)
				win32_post_ext(down ? WM_RBUTTONDOWN : WM_RBUTTONUP, 0, lp);
			break;
		}
		case SDL_KEYDOWN:
		case SDL_KEYUP: {
			if (ev.type == SDL_KEYDOWN && ev.key.repeat)
				break;
			if (present_hotkey(&ev.key)) { /* port hotkeys: CRT, fullscreen */
				cursor_check = 1; /* the image may have moved */
				break;
			}
			uint32_t vk = sdl_to_vk(ev.key.keysym.sym);
			if (vk)
				win32_post_ext(ev.type == SDL_KEYDOWN ? WM_KEYDOWN : WM_KEYUP, vk, 0);
			break;
		}
		case SDL_WINDOWEVENT:
			if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
				win32_post_ext(WM_ACTIVATEAPP, 1, 0);
			else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST && pause_inactive())
				win32_post_ext(WM_ACTIVATEAPP, 0, 0);
			break;
		}
	}
	if (cursor_check)
		win32_cursor_update();
}

static int pop_message(struct msg *m)
{
	if (!q_count)
		return 0;
	*m = queue[q_head];
	q_head = (q_head + 1) % QUEUE_LEN;
	q_count--;
	return 1;
}

static uint32_t dispatch(const struct msg *m)
{
	if (!handlers.wndproc)
		return 0;
	uint32_t r = handlers.wndproc(HWND_MAIN, m->msg, m->wparam, m->lparam);
	if (getenv("NANPA2_MSGLOG") && m->msg != WM_MOUSEMOVE)
		fprintf(stderr, "msg %04x %x -> state %02x frame %u\n", m->msg, m->wparam,
			RT(input_bits), RT(frame));
	return r;
}

/* ---- timer ---- */

int win32_timer_running(void)
{
	return timer_on;
}

/* number of timer callbacks run so far */
static uint32_t timer_ticks;

uint32_t win32_timer_ticks(void)
{
	return timer_ticks;
}

void win32_run_timer(void)
{
	if (!timer_on)
		return;
	uint64_t now = clock_timer_ms();
	/* at most 8 ticks of catching up (more at higher speeds) */
	int n = 0, max = 8 * (clock_speed() > 1 ? (int)clock_speed() + 1 : 1);
	while (now >= timer_next && n < max) {
		handlers.timer();
		timer_ticks++;
		timer_next += 16;
		n++;
	}
	if (now >= timer_next)
		timer_next = now + 16; /* fell far behind: drop ticks */
}

void probe_vm_fetch(uint32_t off)
{
	static int vm = -1;
	if (vm < 0)
		vm = getenv("NANPA2_VMTRACE") != NULL;
	dev_vm_fetch(off);
	if (vm)
		fprintf(stderr, "vm %04x op %02x %02x %02x frame %u us %llu\n", off, rd8(0x435e78 + off),
			rd8(0x435e79 + off), rd8(0x435e7a + off), RT(frame), (unsigned long long)clock_us());
}

void probe_hit_table(uint32_t table)
{
	dev_hit_table(table);
}

/* native 0x401bf0: one PeekMessage/DispatchMessage round; 0 on WM_QUIT */
int win_pump(void)
{
	/* PeekMessage on Windows 95 costs a 486 some hundreds of instructions;
	 * charging it keeps the throttle from running the game's pump loops
	 * (0x4075c6, 0x4074d5, 0x40609c) millions of times a second */
	clock_add(PACE_PUMP_US);
	status_main();
	clock_throttle();
	win32_run_timer();
	replay_pump();
	screen_present(0);
	/* the host event queue at most once a millisecond (deterministic mode:
	 * collect_events counts pumps instead) */
	static uint32_t last_collect;
	uint32_t now = SDL_GetTicks();
	if (replay_det() || now != last_collect) {
		last_collect = now;
		collect_events();
	}
	struct msg m;
	if (pop_message(&m)) {
		if (m.msg == WM_QUIT)
			return 0;
		dispatch(&m);
	}
	return 1;
}

uint32_t win_open(const struct win_handlers *h)
{
	handlers = *h;
	return HWND_MAIN;
}

void win_show(void)
{
	/* Windows activates the new full-screen window; the game only runs its timer after
	 * WM_ACTIVATEAPP */
	win32_post_message(WM_ACTIVATEAPP, 1, 0);
}

void win_post(uint32_t msg, uint32_t wparam, uint32_t lparam)
{
	win32_post_message(msg, wparam, lparam);
}

void win_timer_start(void)
{
	timer_on = 1;
	timer_next = clock_timer_ms() + 16;
}

void win_timer_stop(void)
{
	timer_on = 0;
}

void win_warp_cursor(int x, int y)
{
	/* screen coordinates of the 640x480 display */
	SDL_Window *w = SDL_GetMouseFocus();
	if (w) {
		int wx, wy;
		present_screen_to_window(x, y, &wx, &wy);
		SDL_WarpMouseInWindow(w, wx, wy);
	}
}

void win_message_box(const char *text, const char *title)
{
	fprintf(stderr, "MessageBox (Big5) %s: %s\n", title, text);
}
