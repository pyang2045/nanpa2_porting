/* Platform API: the window, the message queue, the 16 ms timer and the cursor (platform/window.h). */
#include "platform/surface.h"
#include "platform/window.h"
#include "game/ctl.h"
#include "game/pace.h"
#include "platform/clock.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hooks.h"
#include "platform/present.h"
#include "platform/sound.h"
#include "platform/offline.h"
#include "game/startup.h"

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

static int mouse_known, mouse_px, mouse_py;

int win_mouse_pos(int *x, int *y)
{
	*x = mouse_px;
	*y = mouse_py;
	return mouse_known;
}

void win_post(uint32_t msg, uint32_t wparam, uint32_t lparam)
{
	if (msg >= 0x200 && msg <= 0x208) { /* WM_MOUSEMOVE .. WM_RBUTTONDBLCLK: lparam is y << 16 | x */
		mouse_known = 1;
		mouse_px = (int16_t)(lparam & 0xffff);
		mouse_py = (int16_t)(lparam >> 16);
	}
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

void win_post_ext(uint32_t msg, uint32_t wparam, uint32_t lparam)
{
	if (msg == WM_RBUTTONDOWN)
		clock_lat("post");
	if (hook_external(msg, wparam, lparam))
		win_post(msg, wparam, lparam);
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
	if (k >= SDLK_A && k <= SDLK_Z)
		return 'A' + (uint32_t)(k - SDLK_A);
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
void win_cursor_update(void)
{
	SDL_Window *w = SDL_GetMouseFocus();
	int over = 0;
	if (w && (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS)) {
		int sx, sy;
		float fx, fy;
		SDL_GetMouseState(&fx, &fy);
		over = present_window_to_screen((int)fx, (int)fy, &sx, &sy);
	}
	cursor_over = over;
	int shown = cursor_count >= 0 || !over;
	if (shown != cursor_shown) {
		cursor_shown = shown;
		if (shown)
			SDL_ShowCursor();
		else
			SDL_HideCursor();
	}
}

int win_show_cursor(int show)
{
	cursor_count += show ? 1 : -1;
	win_cursor_update();
	return cursor_count;
}

void win_cursor_state(int *count, int *shown, int *over_image)
{
	*count = cursor_count;
	*shown = SDL_CursorVisible();
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

#ifdef SDL_PLATFORM_IOS
/* ---- touch (iOS/iPadOS) ----
 * SDL's own touch-to-mouse events are off (win_open): a one-finger tap is a left click, a two-finger
 * tap a right click (the game's cancel/menu button), a tap of three or more fingers cycles the display modes (F12 on
 * a keyboard); holding or dragging one finger keeps the left button down. The left press waits until
 * the finger lifts, moves or is held, so that more fingers can still turn the touch into a right
 * click. Finger positions are window points, mapped to the screen by the presenter's letterbox
 * (mouse_lparam), like mouse events. */
#define TAP_HOLD_MS 150  /* one finger held this long presses the left button */
#define TAP_SLOP 12      /* ... or moved this far (window points) */
#define TAP_MAX_MS 500   /* a multi-finger tap is shorter than this */
#define CLICK_MS 100     /* a tap's button is held this long: the game polls the state */

static struct {
	int fingers, max_fingers, left_down;
	SDL_FingerID primary;
	int x, y, x0, y0;
	uint32_t t0;
} touch;
static uint32_t up_msg, up_lparam, up_due; /* the delayed button release */
static int in_background;

static void touch_point(const SDL_TouchFingerEvent *f, int *x, int *y)
{
	SDL_Window *w = SDL_GetWindowFromID(f->windowID);
	int ww = 1, wh = 1;
	if (w)
		SDL_GetWindowSize(w, &ww, &wh);
	*x = (int)(f->x * (float)ww);
	*y = (int)(f->y * (float)wh);
}

static void flush_up(void)
{
	if (up_msg) {
		win_post_ext(up_msg, 0, up_lparam);
		up_msg = 0;
	}
}

static void click(uint32_t down, uint32_t up, int x, int y)
{
	flush_up();
	uint32_t lp = mouse_lparam(x, y);
	win_post_ext(WM_MOUSEMOVE, 0, lp);
	win_post_ext(down, 0, lp);
	up_msg = up;
	up_lparam = lp;
	up_due = SDL_GetTicks() + CLICK_MS;
}

static void touch_reset(void)
{
	if (touch.left_down)
		win_post_ext(WM_LBUTTONUP, 0, mouse_lparam(touch.x, touch.y));
	memset(&touch, 0, sizeof touch);
}

static void touch_press_left(void)
{
	flush_up();
	win_post_ext(WM_LBUTTONDOWN, 0, mouse_lparam(touch.x, touch.y));
	touch.left_down = 1;
}

static void touch_event(const SDL_TouchFingerEvent *f, int type)
{
	int x, y;
	touch_point(f, &x, &y);
	uint32_t now = SDL_GetTicks();
	if (type == SDL_EVENT_FINGER_DOWN) {
		if (touch.fingers == 0) {
			memset(&touch, 0, sizeof touch);
			touch.primary = f->fingerID;
			touch.x = touch.x0 = x;
			touch.y = touch.y0 = y;
			touch.t0 = now;
			flush_up();
			win_post_ext(WM_MOUSEMOVE, 0, mouse_lparam(x, y));
		}
		touch.fingers++;
		if (touch.fingers > touch.max_fingers)
			touch.max_fingers = touch.fingers;
	} else if (type == SDL_EVENT_FINGER_MOTION) {
		if (f->fingerID != touch.primary)
			return;
		touch.x = x;
		touch.y = y;
		win_post_ext(WM_MOUSEMOVE, 0, mouse_lparam(x, y));
		int dx = x - touch.x0, dy = y - touch.y0;
		if (touch.max_fingers == 1 && !touch.left_down && dx * dx + dy * dy > TAP_SLOP * TAP_SLOP)
			touch_press_left();
	} else if (type == SDL_EVENT_FINGER_UP) {
		if (touch.fingers <= 0)
			return;
		if (f->fingerID == touch.primary) {
			touch.x = x;
			touch.y = y;
		}
		if (--touch.fingers > 0)
			return;
		/* all fingers are up: what was it? */
		if (touch.left_down)
			win_post_ext(WM_LBUTTONUP, 0, mouse_lparam(touch.x, touch.y));
		else if (touch.max_fingers == 1)
			click(WM_LBUTTONDOWN, WM_LBUTTONUP, touch.x, touch.y);
		else if (now - touch.t0 < TAP_MAX_MS) {
			if (touch.max_fingers == 2)
				click(WM_RBUTTONDOWN, WM_RBUTTONUP, touch.x0, touch.y0);
			else /* three or more fingers: iPadOS takes some four-finger gestures, so they count as three */
				present_cycle_mode();
		}
		memset(&touch, 0, sizeof touch);
	}
}

/* every pump: a held finger presses, a delayed release comes due */
static void touch_tick(void)
{
	uint32_t now = SDL_GetTicks();
	if (touch.fingers == 1 && touch.max_fingers == 1 && !touch.left_down && now - touch.t0 >= TAP_HOLD_MS)
		touch_press_left();
	if (up_msg && (int32_t)(now - up_due) >= 0)
		flush_up();
}

/* SDL_EVENT_WILL_ENTER_BACKGROUND must be handled at once (the OS stops graphics work when it returns),
 * hence an event watch. The game timer needs nothing: it drops the ticks it missed. */
static bool lifecycle_watch(void *ud, SDL_Event *e)
{
	static int music_paused;
	switch (e->type) {
	case SDL_EVENT_WILL_ENTER_BACKGROUND:
		in_background = 1;
		touch_reset();
		if (!music_paused) {
			sound_music_pause(1);
			music_paused = 1;
		}
		break;
	case SDL_EVENT_DID_ENTER_FOREGROUND:
		in_background = 0;
		if (music_paused) {
			sound_music_pause(0);
			music_paused = 0;
		}
		break;
	}
	return true;
}
#endif

/* Move SDL events into the native queue. */
static void collect_events(void)
{
	SDL_Event ev;
	/* deterministic: the scripts and the bot run at every pump, and the host
	 * queue every 256th (messages carry the pump they came at) */
	static unsigned n;
	if (hook_det()) {
		if (!hook_replaying())
			hook_inject();
		if (n++ & 255)
			return;
	} else {
		hook_inject();
	}
	/* scripted runs only take SDL_EVENT_QUIT from the host, so the Cocoa event
	 * pump (most of their host CPU) can run every 16 ms */
	if (hook_scripted() && !hook_det()) {
		static uint32_t last;
		uint32_t now = SDL_GetTicks();
		if (now - last < 16)
			return;
		last = now;
	}
	int cursor_check = 0;
	while (SDL_PollEvent(&ev)) {
		if (ev.type == SDL_EVENT_MOUSE_MOTION ||
		    (ev.type >= SDL_EVENT_WINDOW_FIRST && ev.type <= SDL_EVENT_WINDOW_LAST))
			cursor_check = 1;
		/* scripted runs: the real cursor, keys and focus changes on a shared
		 * desktop would move the injected mouse or stop the timer
		 * (WM_ACTIVATEAPP 0 clears 0x4ac4ec) */
		if (hook_scripted() && ev.type != SDL_EVENT_QUIT)
			continue;
		switch (ev.type) {
		case SDL_EVENT_QUIT:
			win_post_ext(WM_QUIT, 0, 0);
			break;
		case SDL_EVENT_MOUSE_MOTION:
			win_post_ext(WM_MOUSEMOVE, 0, mouse_lparam((int)ev.motion.x, (int)ev.motion.y));
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP: {
			int down = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
			uint32_t lp = mouse_lparam((int)ev.button.x, (int)ev.button.y);
			if (ev.button.button == SDL_BUTTON_LEFT)
				win_post_ext(down ? WM_LBUTTONDOWN : WM_LBUTTONUP, 0, lp);
			else if (ev.button.button == SDL_BUTTON_RIGHT) {
				win_post_ext(down ? WM_RBUTTONDOWN : WM_RBUTTONUP, 0, lp);
			}
			break;
		}
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP: {
			if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.repeat)
				break;
			if (present_hotkey(&ev.key)) { /* port hotkeys: CRT, fullscreen */
				cursor_check = 1; /* the image may have moved */
				break;
			}
			uint32_t vk = sdl_to_vk(ev.key.key);
			if (vk)
				win_post_ext(ev.type == SDL_EVENT_KEY_DOWN ? WM_KEYDOWN : WM_KEYUP, vk, 0);
			break;
		}
#ifdef SDL_PLATFORM_IOS
		case SDL_EVENT_FINGER_DOWN:
		case SDL_EVENT_FINGER_MOTION:
		case SDL_EVENT_FINGER_UP:
			touch_event(&ev.tfinger, ev.type);
			break;
#endif
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			win_post_ext(WM_ACTIVATEAPP, 1, 0);
			break;
		case SDL_EVENT_WINDOW_FOCUS_LOST:
#ifdef SDL_PLATFORM_IOS
			touch_reset();
#endif
			if (pause_inactive())
				win_post_ext(WM_ACTIVATEAPP, 0, 0);
			break;
		}
	}
	if (cursor_check)
		win_cursor_update();
#ifdef SDL_PLATFORM_IOS
	touch_tick();
#endif
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
	if (m->msg == WM_RBUTTONDOWN)
		clock_lat("dispatch");
	if (!handlers.wndproc)
		return 0;
	uint32_t r = handlers.wndproc(HWND_MAIN, m->msg, m->wparam, m->lparam);
	if (getenv("NANPA2_MSGLOG") && m->msg != WM_MOUSEMOVE)
		fprintf(stderr, "msg %04x %x -> state %02x frame %u\n", m->msg, m->wparam,
			RT(input_bits), RT(frame));
	return r;
}

/* ---- timer ---- */

int win_timer_running(void)
{
	return timer_on;
}

/* number of timer callbacks run so far */
static uint32_t timer_ticks;

uint32_t win_timer_ticks(void)
{
	return timer_ticks;
}

void win_run_timer(void)
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
	if (now >= timer_next) {
		clock_lat("tick_drop");
		timer_next = now + 16; /* fell far behind: drop ticks */
	}
}

void win_reset(void)
{
	q_head = q_count = 0;
	timer_on = 0;
	timer_next = 0;
	timer_ticks = 0;
	mouse_known = 0;
	cursor_count = 0;
	cursor_shown = -1;
	memset(&handlers, 0, sizeof handlers);
}

static uint32_t idle_run; /* consecutive pumps that found no tick due (offline mode) */

/* native 0x401bf0: one PeekMessage/DispatchMessage round; 0 on WM_QUIT */
int win_pump(void)
{
	/* PeekMessage on Windows 95 costs a 486 some hundreds of instructions;
	 * charging it keeps the throttle from running the game's pump loops
	 * (0x4075c6, 0x4074d5, 0x40609c) millions of times a second */
#ifdef SDL_PLATFORM_IOS
	while (in_background) { /* no graphics work in the background (the OS suspends the process anyway) */
		SDL_PumpEvents();
		SDL_Delay(50);
	}
#endif
	clock_add(PACE_PUMP_US);
	hook_beat();
	clock_throttle();
	uint32_t ticks_before = timer_ticks;
	win_run_timer();
	/* offline: a pump that found no tick due only waits for the next one, so the virtual clock
	 * goes there at once (the same game, minus the host spinning); a quit request is the only
	 * host input (collect_events) */
	if (timer_ticks != ticks_before)
		idle_run = 0;
	else if (offline_fast_forward() && timer_on && ++idle_run >= OFFLINE_IDLE_PUMPS)
		clock_skip_to_ms(timer_next);
	hook_pump();
	screen_present(0);
	/* the host event queue at most once a millisecond (deterministic mode:
	 * collect_events counts pumps instead) */
	static uint32_t last_collect;
	int det = hook_det();
	uint32_t now = det ? 0 : SDL_GetTicks();
	if (det || now != last_collect) {
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
#ifdef SDL_PLATFORM_IOS
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0"); /* touch_event does it */
	SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
	SDL_DisableScreenSaver(); /* the idle timer would dim the screen during long cutscenes */
	SDL_AddEventWatch(lifecycle_watch, NULL);
#endif
	return HWND_MAIN;
}

void win_show(void)
{
	/* Windows activates the new full-screen window; the game only runs its timer after
	 * WM_ACTIVATEAPP */
	win_post(WM_ACTIVATEAPP, 1, 0);
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
	fprintf(stderr, "MessageBox %s: %s\n", title, text);
	if (!getenv("NANPA2_HEADLESS") && !getenv("NANPA2_INPUT"))
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, NULL);
}
