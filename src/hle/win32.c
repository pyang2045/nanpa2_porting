/* USER32/GDI32/KERNEL32/WINMM imports, the message queue, the 16 ms timer. */
#include "hle.h"
#include "replay.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev.h"
#include "status.h"
#include "platform/present.h"
#include "game/state.h"

#define HWND_MAIN 0x00001000u
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

uint32_t win32_wndproc;
static int timer_on;
static uint64_t timer_next; /* in 1/turbo ms */
/* NANPA2_TURBO=N (testing): the 16 ms timer, which is the game's clock, and the
 * emulated CPU run N times faster than real time, so long scripted runs finish sooner */
static int turbo = 1;
/* the native ShowCursor display counter (0 at start; shown while >= 0) */
static int cursor_count;
static int cursor_shown = -1, cursor_over; /* host cursor state last set */

enum {
	F_ShowCursor = 1, F_DispatchMessageA, F_TranslateMessage, F_PeekMessageA, F_SetCursorPos,
	F_SetWindowPos, F_GetSystemMetrics, F_MsgWaitForMultipleObjects, F_PostMessageA,
	F_PostQuitMessage, F_DestroyWindow, F_LoadIconA, F_LoadCursorA, F_RegisterClassA,
	F_CreateWindowExA, F_MessageBoxA, F_ShowWindow, F_UpdateWindow, F_SetFocus,
	F_DefWindowProcA, F_GetMessageA, F_SetWindowLongA,
	F_DeleteObject, F_TextOutA, F_CreateFontA, F_GetStockObject, F_SelectObject,
	F_SetTextColor, F_SetBkColor, F_SetBkMode,
	F_timeSetEvent, F_timeKillEvent, F_mciSendStringA,
	F_GetDriveTypeA, F_CreateEventA, F_CloseHandle,
	F_DirectDrawCreate, F_DirectSoundCreate,
};

static const struct {
	const char *name;
	int id, nargs;
} table[] = {
	{ "ShowCursor", F_ShowCursor, 1 },
	{ "DispatchMessageA", F_DispatchMessageA, 1 },
	{ "TranslateMessage", F_TranslateMessage, 1 },
	{ "PeekMessageA", F_PeekMessageA, 5 },
	{ "SetCursorPos", F_SetCursorPos, 2 },
	{ "SetWindowPos", F_SetWindowPos, 7 },
	{ "GetSystemMetrics", F_GetSystemMetrics, 1 },
	{ "MsgWaitForMultipleObjects", F_MsgWaitForMultipleObjects, 5 },
	{ "PostMessageA", F_PostMessageA, 4 },
	{ "PostQuitMessage", F_PostQuitMessage, 1 },
	{ "DestroyWindow", F_DestroyWindow, 1 },
	{ "LoadIconA", F_LoadIconA, 2 },
	{ "LoadCursorA", F_LoadCursorA, 2 },
	{ "RegisterClassA", F_RegisterClassA, 1 },
	{ "CreateWindowExA", F_CreateWindowExA, 12 },
	{ "MessageBoxA", F_MessageBoxA, 4 },
	{ "ShowWindow", F_ShowWindow, 2 },
	{ "UpdateWindow", F_UpdateWindow, 1 },
	{ "SetFocus", F_SetFocus, 1 },
	{ "DefWindowProcA", F_DefWindowProcA, 4 },
	{ "GetMessageA", F_GetMessageA, 4 },
	{ "SetWindowLongA", F_SetWindowLongA, 3 },
	{ "DeleteObject", F_DeleteObject, 1 },
	{ "TextOutA", F_TextOutA, 5 },
	{ "CreateFontA", F_CreateFontA, 14 },
	{ "GetStockObject", F_GetStockObject, 1 },
	{ "SelectObject", F_SelectObject, 2 },
	{ "SetTextColor", F_SetTextColor, 2 },
	{ "SetBkColor", F_SetBkColor, 2 },
	{ "SetBkMode", F_SetBkMode, 2 },
	{ "timeSetEvent", F_timeSetEvent, 5 },
	{ "timeKillEvent", F_timeKillEvent, 1 },
	{ "mciSendStringA", F_mciSendStringA, 4 },
	{ "GetDriveTypeA", F_GetDriveTypeA, 1 },
	{ "CreateEventA", F_CreateEventA, 4 },
	{ "CloseHandle", F_CloseHandle, 1 },
	{ "DirectDrawCreate", F_DirectDrawCreate, 3 },
	{ "#1", F_DirectSoundCreate, 3 },
};

uint32_t win32_import(const char *dll, const char *name, int *nargs, int *known)
{
	for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
		if (!strcmp(table[i].name, name)) {
			*nargs = table[i].nargs;
			*known = 1;
			return (uint32_t)table[i].id;
		}
	*nargs = 0;
	*known = 0;
	return 0;
}

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

int win32_show_cursor(int show)
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
	if (!win32_wndproc)
		return 0;
	uint32_t args[4] = { HWND_MAIN, m->msg, m->wparam, m->lparam };
	uint32_t r = x86_call(win32_wndproc, 4, args);
	if (getenv("NANPA2_MSGLOG") && m->msg != WM_MOUSEMOVE)
		fprintf(stderr, "msg %04x %x -> state %02x frame %u\n", m->msg, m->wparam,
			RT(input_bits), RT(frame));
	return r;
}

/* ---- timer ---- */

void hle_timer_callback(void);

int win32_timer_running(void)
{
	return timer_on;
}

/* game-timer clock in 1/turbo ms (SDL_GetTicks wraps only after 49 days) */
static uint32_t timer_ticks;

uint32_t win32_timer_ticks(void)
{
	return timer_ticks;
}

static uint64_t timer_now(void)
{
	/* deterministic mode: the instruction count is the clock (turbo off) */
	if (replay_det())
		return replay_ms();
	return (uint64_t)SDL_GetTicks() * (uint64_t)turbo;
}

void win32_run_timer(void)
{
	if (!timer_on)
		return;
	uint64_t now = timer_now();
	int n = 0;
	while (now >= timer_next && n < 8 * turbo) {
		hle_timer_callback();
		timer_ticks++;
		timer_next += 16;
		n++;
	}
	if (now >= timer_next)
		timer_next = now + 16; /* fell far behind: drop ticks */
}

/* ---- emulated CPU speed ----
 * Budget: instructions executed since the base point may not exceed the
 * real time elapsed times the rate. Ahead of it, sleep; far behind (HLE
 * work, a stall, an unthrottled stretch), move the base point so the game
 * never bursts to catch up. Timer-paced waits (frame counter 0x446292) are
 * unaffected: they spin, throttled, until the real-time timer ticks. */
static double cpu_ips;
static uint64_t base_count, base_ticks;

void x86_set_speed(double ips)
{
	cpu_ips = ips * turbo;
	base_count = x86_icount;
	base_ticks = SDL_GetPerformanceCounter();
}

void x86_throttle(void)
{
	if (cpu_ips <= 0 || replay_replaying())
		return;
	uint64_t now = SDL_GetPerformanceCounter();
	double freq = (double)SDL_GetPerformanceFrequency();
	double real = (double)(now - base_ticks) / freq;
	double virt = (double)(x86_icount - base_count) / cpu_ips;
	double ahead = virt - real;
	if (ahead >= 0.002) {
		SDL_Delay((uint32_t)(ahead * 1000.0));
	} else if (ahead < -0.05) {
		base_ticks = now;
		base_count = x86_icount;
	}
}

void x86_probe(uint32_t pc)
{
	static int on = -1, vm = -1;
	if (on < 0) {
		on = getenv("NANPA2_PROBE") != NULL;
		vm = getenv("NANPA2_VMTRACE") != NULL;
	}
	if (pc == 0x40616e) { /* script VM opcode fetch: esi = offset in 0x435e78 */
		dev_vm_fetch(cpu.r[ESI]);
		if (vm)
			fprintf(stderr, "vm %04x op %02x %02x %02x frame %u ic %llu\n", cpu.r[ESI],
				rd8(0x435e78 + cpu.r[ESI]), rd8(0x435e79 + cpu.r[ESI]),
				rd8(0x435e7a + cpu.r[ESI]), RT(frame), (unsigned long long)x86_icount);
		return;
	}
	if (pc == 0x4071bb) { /* script hit test 0x4071a4: edi = rectangle table */
		dev_hit_table(cpu.r[EDI]);
		return;
	}
	if (on)
		fprintf(stderr, "probe %06x %u ms frame %u icount %llu\n", pc, dev_now(), RT(frame), (unsigned long long)x86_icount);
}

void x86_poll(void)
{
	X86_LOCKSTEP_POINT(LS_POLL, x86_poll_pc); /* before the timer writes memory */
	static uint32_t last_log;
	static int log = -1;
	if (log < 0)
		log = getenv("NANPA2_LOG") != NULL;
	if (log && SDL_GetTicks() - last_log > 1000) {
		last_log = SDL_GetTicks();
		fprintf(stderr, "poll at %08x frame %u esi %08x eax %08x [esp] %08x\n", x86_poll_pc, RT(frame), cpu.r[ESI], cpu.r[EAX], rd32(cpu.r[ESP]));
	}
	x86_poll_countdown = 2000;
	status_main();
	x86_throttle();
	win32_run_timer();
	ddraw_present(0);
	SDL_PumpEvents();
}

/* native 0x401bf0: one PeekMessage/DispatchMessage round; 0 on WM_QUIT */
int win32_pump(void)
{
	/* PeekMessage on Windows 95 costs a 486 some hundreds of instructions;
	 * charging it keeps the throttle from running the game's pump loops
	 * (0x4075c6, 0x4074d5, 0x40609c) millions of times a second */
	X86_COUNT(500);
	status_main();
	x86_throttle();
	win32_run_timer();
	replay_pump();
	ddraw_present(0);
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

void win32_init(void)
{
	cursor_count = 0;
	cursor_shown = -1;
	const char *t = getenv("NANPA2_TURBO");
	turbo = t && atoi(t) > 1 && !replay_det() ? atoi(t) : 1;
}

int win32_call_import(int id)
{
	uint32_t r = 0;
	switch (id) {
	case F_ShowCursor:
		r = (uint32_t)win32_show_cursor(hle_arg(0) != 0);
		break;
	case F_SetCursorPos: {
		/* screen coordinates of the 640x480 display */
		SDL_Window *w = SDL_GetMouseFocus();
		if (w) {
			int x, y;
			present_screen_to_window((int)hle_arg(0), (int)hle_arg(1), &x, &y);
			SDL_WarpMouseInWindow(w, x, y);
		}
		r = 1;
		break;
	}
	case F_GetSystemMetrics:
		r = hle_arg(0) == 0 ? SCREEN_W : hle_arg(0) == 1 ? SCREEN_H : 0;
		break;
	case F_PostMessageA:
		win32_post_message(hle_arg(1), hle_arg(2), hle_arg(3));
		r = 1;
		break;
	case F_PostQuitMessage:
	case F_DestroyWindow:
		win32_post_message(WM_QUIT, 0, 0);
		r = 1;
		break;
	case F_PeekMessageA: {
		/* only the logo player's loop would use these; never reached */
		r = 0;
		break;
	}
	case F_GetMessageA:
		r = 0;
		break;
	case F_ShowWindow:
		/* Windows activates the new full-screen window; the game only runs
		 * its timer (0x4ac4ec) after WM_ACTIVATEAPP */
		win32_post_message(WM_ACTIVATEAPP, 1, 0);
		r = 0;
		break;
	case F_DispatchMessageA:
	case F_TranslateMessage:
	case F_SetWindowPos:
	case F_UpdateWindow:
	case F_SetFocus:
	case F_DefWindowProcA:
	case F_SetWindowLongA:
	case F_MsgWaitForMultipleObjects:
		r = 0;
		break;
	case F_LoadIconA:
	case F_LoadCursorA:
		r = 0x2000;
		break;
	case F_RegisterClassA:
		win32_wndproc = rd32(hle_arg(0) + 4);
		r = 0xc000;
		break;
	case F_CreateWindowExA:
		r = HWND_MAIN;
		break;
	case F_MessageBoxA: {
		char text[256], title[64];
		hle_strcpy_in(text, hle_arg(1), sizeof text);
		hle_strcpy_in(title, hle_arg(2), sizeof title);
		fprintf(stderr, "MessageBox (Big5) %s: %s\n", title, text);
		r = 1;
		break;
	}
	case F_DeleteObject:
		r = 1;
		break;
	case F_TextOutA:
		/* emulated CPU time: GDI TrueType text on Windows 95 / 486, about
		 * 0.2 ms a (cached) glyph */
		X86_COUNT(5000u * hle_arg(4));
		text_out(hle_arg(0), (int)hle_arg(1), (int)hle_arg(2), hle_arg(3), (int)hle_arg(4));
		r = 1;
		break;
	case F_CreateFontA:
		r = text_create_font((int)hle_arg(0), (int)hle_arg(4));
		break;
	case F_GetStockObject:
		r = 0x3000 + hle_arg(0);
		break;
	case F_SelectObject:
		text_select(hle_arg(0), hle_arg(1));
		r = 0;
		break;
	case F_SetTextColor:
		text_set_color(hle_arg(0), hle_arg(1), 0xffffffffu, -1);
		r = 0;
		break;
	case F_SetBkColor:
		text_set_color(hle_arg(0), 0xffffffffu, hle_arg(1), -1);
		r = 0;
		break;
	case F_SetBkMode:
		text_set_color(hle_arg(0), 0xffffffffu, 0xffffffffu, (int)hle_arg(1));
		r = 1;
		break;
	case F_timeSetEvent:
		timer_on = 1;
		timer_next = timer_now() + 16;
		r = 1;
		break;
	case F_timeKillEvent:
		timer_on = 0;
		r = 0;
		break;
	case F_mciSendStringA:
		r = 1; /* MCIERR: the logo movie is skipped */
		break;
	case F_GetDriveTypeA: {
		/* the game folder is the CD in drive Q: */
		char root[8];
		hle_strcpy_in(root, hle_arg(0), sizeof root);
		r = ((root[0] | 0x20) == 'q') ? 5 /* DRIVE_CDROM */ : 1 /* DRIVE_NO_ROOT_DIR */;
		break;
	}
	case F_CreateEventA:
		r = 0x4000;
		break;
	case F_CloseHandle:
		r = 1;
		break;
	case F_DirectDrawCreate:
		wr32(hle_arg(1), ddraw_create());
		r = 0;
		break;
	case F_DirectSoundCreate:
		r = 0x80004005u; /* E_FAIL: sound goes through sound.c */
		break;
	default:
		return 0;
	}
	cpu.r[EAX] = r;
	return 1;
}
