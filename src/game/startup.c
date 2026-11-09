/*
 * Start-up and shutdown (WinMain 0x402090, the CD search 0x402590, the display, sound, fonts and
 * timer set-up 0x402620, the shutdown 0x402a20) and the window procedure 0x4021c0 with its repaint
 * 0x402570.
 *
 * game_main opens the window, looks for the game CD, plays the logo, sets the display up, runs the
 * script VM (game_vm_run) and tears everything down when the VM ends. The window, the display
 * (surfaces and palette), the fonts, the timer and the sound are the platform's (src/platform):
 * the surfaces are named by handles kept in G.rt.surface. Semantics: re-notes.md "Start-up" and
 * "Window procedure".
 */
#include <stdio.h>
#include <string.h>

#include "game/ctl.h"
#include "game/startup.h"
#include "game/state.h"
#include "game/vm.h"
#include "hle/hle.h"
#include "hle/logo.h"
#include "platform/files.h"
#include "platform/sound.h"
#include "platform/surface.h"
#include "platform/window.h"
#include "game/gmem.h"

/* the game's globals */
#define CD_PREFIX 0x431030u    /* "X:\" of the CD found, or empty */
#define SURFACES 0x44629eu     /* u32[4]: 0 the screen, 1-3 off-screen */
#define PITCHES 0x4462aeu

#define MSG_NO_CD 0x431140u
#define MSG_NO_CD_TITLE 0x431158u

/* ---- the CD search (0x402590) ---- */

/* Look for the game CD: the first CD-ROM drive letter whose
 * SPEECH\A\A10930.WAV opens sets the prefix at CD_PREFIX; else the file is
 * tried in the current folder (prefix ""). Returns 1 if found. */
static uint32_t cd_search(void)
{
	FILE *found = NULL;
	for (uint32_t drive = 0; drive < 0x1a; drive++) {
		wr8(CD_PREFIX, 'A' + drive);
		if (!files_drive_is_cd((char)('A' + drive)))
			continue;
		char path[64];
		snprintf(path, sizeof path, "%sSPEECH\\A\\A10930.WAV", (const char *)g_ptr(CD_PREFIX, 1));
		found = files_open(path, "rb");
		if (found)
			break;
	}
	if (!found) {
		wr8(CD_PREFIX, 0);
		found = files_open("SPEECH\\A\\A10930.WAV", "rb");
		if (!found)
			return 0;
	}
	fclose(found);
	return 1;
}

/* ---- display, sound and timer set-up (0x402620) ---- */

#define COLOUR_KEY 8u /* the transparent colour of the surfaces */

/* The screen (a 640x480 8-bit surface; letterboxed: the picture sits 40 lines down), the
 * all-black palette, three off-screen 640x400 surfaces, the 640x480 scratch surface for text,
 * the three fonts (15, 24 and 32 pixels, Big5) and the 16 ms timer. */
static void display_init(void)
{
	win_show_cursor(1);
	sound_init();
	SC_SET(letterbox, 1);
	int pitch;
	uint32_t screen = surf_create(640, 480, 1);
	G.rt.surface[0] = screen;
	surf_lock(screen, &pitch);
	G.rt.pitch[0] = (uint32_t)pitch;
	surf_set_colorkey(screen, 1, COLOUR_KEY, COLOUR_KEY);
	memset(G.sc.pal_entries, 0, sizeof G.sc.pal_entries);
	pal_set(0, 256, G.sc.pal_entries);
	for (uint32_t i = 0; i < 3; i++) {
		uint32_t s = surf_create(640, 400, 0);
		G.rt.surface[1 + i] = s;
		surf_set_colorkey(s, 1, COLOUR_KEY, COLOUR_KEY);
		G.rt.pitch[1 + i] = 640;
	}
	SC_SET(scratch_surface, surf_create(640, 480, 0));
	static const struct { int height, weight; } fonts[3] = { { -15, 700 }, { -24, 700 }, { -32, 400 } };
	for (int i = 0; i < 3; i++)
		G.sc.font[i] = text_font_create(fonts[i].height, fonts[i].weight);
	win_timer_start();
	win_show_cursor(0);
}

/* ---- shutdown (0x402a20) ---- */

static void shutdown(void)
{
	win_timer_stop();
	/* the surfaces, palette and fonts go with the process */
	sound_shutdown();
	win_show_cursor(1);
}

/* ---- WinMain (0x402090) ---- */

/* 0x402090: open the window, find the CD, play the logo, set the display up, show the window, run
 * the script VM (0x4060d4) and shut down when it ends. Returns WinMain's result: 0 no CD, 1 the
 * game ran. */
uint32_t game_main(void)
{
	static const struct win_handlers handlers = { game_wndproc, game_timer_tick };
	win_open(&handlers);
	if (!cd_search()) {
		win_message_box((const char *)g_ptr(MSG_NO_CD, 1), (const char *)g_ptr(MSG_NO_CD_TITLE, 1));
		return 0;
	}
	win_show_cursor(0); /* the first thing the logo player does, whether or not a movie plays */
	logo_play();
	display_init();
	win_show();
	game_vm_run();
	shutdown();
	return 1;
}

/* ---- the window procedure (0x4021c0) ---- */

/* The keys the game reads, as input bits (INPUT_BITS): the arrows, Enter
 * and Space (OK), Escape and Insert (cancel), Ctrl, End. F1 closes the
 * window. */
static uint32_t key_bit(uint32_t vk)
{
	switch (vk) {
	case 0x26: return 0x01; /* up */
	case 0x28: return 0x02; /* down */
	case 0x25: return 0x04; /* left */
	case 0x27: return 0x08; /* right */
	case 0x0d: case 0x20: return 0x10; /* enter, space */
	case 0x1b: case 0x2d: return 0x20; /* escape, insert */
	case 0x11: return 0x40; /* control */
	case 0x23: return 0x80; /* end */
	}
	return 0;
}

static void voice_stop(void)
{
	sound_voice_stop();
}

/* 0x402570: after the pointer moved with the cursor drawn: wait for the
 * vertical blank, redraw the cursor (restore the background, draw it
 * again). The cursor routines are native until the screen module is C. */
static void paint(void)
{
	screen_wait_vblank();
	game_cursor_restore();
	game_cursor_draw();
}

/* The window procedure, stdcall (hwnd, msg, wparam, lparam): returns the
 * result for the message. Called by the HLE message pump (win32.c), which
 * keeps the CPU registers across it as x86_call did. The native registers
 * it leaves (ECX per path, EDX = 0) were never read. */
uint32_t game_wndproc(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
	uint32_t result = 0;
	if (msg == 0x200) { /* WM_MOUSEMOVE */
		uint32_t x = lparam & 0xffff, y = lparam >> 16;
		SC_SET(cursor_x, x);
		SV_SET(mouse_x, x);
		SC_SET(cursor_y, y);
		if (SC(letterbox)) {
			y = (y - 0x28) & 0xffff;
			if ((int16_t)y < 0) {
				y = 0;
			} else if ((int16_t)y > 0x18f) {
				y = 0x18f;
			} else {
			}
		}
		SV_SET(mouse_y, y);
		if (SC(cursor_on)) {
			paint();
		}
		goto done;
	}
	if (msg > 0x200) { /* mouse buttons */
		switch (msg) {
		case 0x201: /* left button down: stop the voice, OK pressed */
		case 0x204: { /* right button down: cancel pressed */
			voice_stop();
			RT_SET(input_bits, RT(input_bits) | (msg == 0x201 ? 0x10 : 0x20));
			goto done;
		}
		case 0x202:
		case 0x205:
			RT_SET(input_bits, RT(input_bits) & ~(msg == 0x202 ? 0x10 : 0x20));
			goto done;
		}
		goto def;
	}
	if (msg == 0x100) { /* WM_KEYDOWN */
		if (wparam - 0xd > 0x63)
			goto done;
		if (wparam == 0x70) { /* F1 */
			win_post(WM_QUIT, 0, 0); /* DestroyWindow */
			goto done;
		}
		uint32_t bit = key_bit(wparam);
		if (bit)
			RT_SET(input_bits, RT(input_bits) | bit);
		goto done;
	}
	if (msg > 0x100) {
		if (msg != 0x101)
			goto def;
		/* WM_KEYUP */
		if (wparam - 0xd > 0x20)
			goto done;
		uint32_t bit = key_bit(wparam);
		if (bit == 0x10 || bit == 0x20)
			voice_stop(); /* Enter, Space, Escape, Insert */
		if (bit)
			RT_SET(input_bits, RT(input_bits) & ~bit);
		goto done;
	}
	if (msg == 2) { /* WM_DESTROY */
		win_post(WM_QUIT, 0, 0); /* PostQuitMessage */
		goto done;
	}
	if (msg != 0x1c) /* WM_ACTIVATEAPP */
		goto def;
	if (wparam == 0) { /* deactivated: pause the sound, stop the game's clock */
		SC_SET(paused, 1);
		SC_SET(active, 0);
		voice_stop();
		sound_music_pause(1);
		goto done;
	}
	if (SC(paused)) {
		sound_music_pause(0);
	}
	SC_SET(active, 1);
	goto done;
def: /* DefWindowProc: nothing the game needs */
	result = 0;
done:
	return result;
}
