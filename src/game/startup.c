/*
 * Stage 5B: start-up and shutdown (WinMain 0x402090, the CD search 0x402590,
 * DirectDraw / surfaces / fonts / timer set-up 0x402620, the shutdown
 * 0x402a20) and the window procedure 0x4021c0 with its repaint 0x402570.
 *
 * WinMain registers the window class, opens a full-screen window, looks for
 * the game CD, plays the logo (the HLE 0x401e20), sets the display up, runs
 * the script VM (game_vm_run) and tears everything down when the VM ends.
 *
 * Windows APIs and DirectDraw methods are called as the native code calls
 * them (hle_invoke through the import table and the objects' vtables), so
 * src/hle serves them unchanged. Semantics: re-notes.md "Start-up" and
 * "Window procedure".
 */
#include "game/ctl.h"
#include "game/startup.h"
#include "game/state.h"
#include "game/vm.h"
#include "x86/x86.h"

/* import address table slots (the loaded image's .idata) */
#define IAT_DDRAWCREATE 0x41b000u
#define IAT_DELETEOBJECT 0x41b010u
#define IAT_CREATEFONT 0x41b018u
#define IAT_GETSTOCKOBJECT 0x41b01cu
#define IAT_GETDRIVETYPE 0x41b068u
#define IAT_SHOWCURSOR 0x41b11cu
#define IAT_GETSYSMETRICS 0x41b134u
#define IAT_POSTQUIT 0x41b140u
#define IAT_DESTROYWINDOW 0x41b144u
#define IAT_LOADICON 0x41b148u
#define IAT_LOADCURSOR 0x41b14cu
#define IAT_REGISTERCLASS 0x41b150u
#define IAT_CREATEWINDOW 0x41b154u
#define IAT_MESSAGEBOX 0x41b158u
#define IAT_SHOWWINDOW 0x41b15cu
#define IAT_UPDATEWINDOW 0x41b160u
#define IAT_SETFOCUS 0x41b164u
#define IAT_DEFWINDOWPROC 0x41b168u
#define IAT_TIMESETEVENT 0x41b178u
#define IAT_TIMEKILLEVENT 0x41b17cu

/* COM vtable offsets */
#define DD_RELEASE 0x08u
#define DD_CREATEPALETTE 0x14u
#define DD_CREATESURFACE 0x18u
#define DD_SETCOOPERATIVELEVEL 0x50u
#define DD_SETDISPLAYMODE 0x54u
#define SURF_LOCK 0x64u
#define SURF_RESTORE 0x6cu
#define SURF_SETCOLORKEY 0x74u
#define SURF_SETPALETTE 0x7cu
#define SURF_UNLOCK 0x80u
#define PAL_RELEASE 0x08u
#define DD_WAITVBLANK 0x58u

/* the game's globals */
#define HWND_MAIN 0x4ab05cu
#define CD_PREFIX 0x431030u    /* "X:\" of the CD found, or empty */
#define DDRAW 0x4ac4f0u
#define PALETTE_OBJ 0x4ac4f4u
#define SURFACES 0x44629eu     /* u32[4]: 0 the screen, 1-3 off-screen */
#define PITCHES 0x4462aeu
#define SCRATCH_SURFACE 0x4ab058u
#define SURFACE_DESC 0x4ac480u
#define DESC_FLAGS (SURFACE_DESC + 4)
#define DESC_HEIGHT (SURFACE_DESC + 8)
#define DESC_WIDTH (SURFACE_DESC + 12)
#define DESC_PITCH (SURFACE_DESC + 16)
#define DESC_CAPS (SURFACE_DESC + 0x68)
#define PALETTE_ENTRIES 0x4ac07cu /* 256 x 4 bytes */
#define FONTS 0x4ac070u         /* u32[3] */

#define CD_FMT 0x431188u        /* "%sSPEECH\A\A10930.WAV" */
#define CD_FALLBACK_FILE 0x431174u /* "SPEECH\A\A10930.WAV" */
#define MODE_READ 0x43103cu
#define WND_CLASS 0x431160u
#define ICON_NAME 0x431168u
#define FONT_FACE 0x4311a0u
#define MSG_NO_CD 0x431140u
#define MSG_NO_CD_TITLE 0x431158u
#define MSG_DD_ERROR 0x4311c4u
#define MSG_MODE_ERROR 0x4311a8u
#define MSG_TITLE_ERROR 0x4311e0u
#define LOGO_ARG 0x431134u


/* ---- calls into imports and COM objects */

static uint32_t stdcall(uint32_t fn, int n, const uint32_t *a)
{
	X86_COUNT((uint64_t)n + 1);
	return hle_invoke(fn, n, a);
}

#define IMPORT(slot, ...) stdcall(rd32(slot), (int)(sizeof((uint32_t[]){ __VA_ARGS__ }) / 4), \
	(const uint32_t[]){ __VA_ARGS__ })
/* method at vtable offset `off` of obj; obj is the first argument */
#define COM(obj, off, ...) stdcall(rd32(rd32(obj) + (off)), \
	(int)(sizeof((uint32_t[]){ (obj), __VA_ARGS__ }) / 4), (const uint32_t[]){ (obj), __VA_ARGS__ })
#define COM1(obj, off) stdcall(rd32(rd32(obj) + (off)), 1, (const uint32_t[]){ (obj) })

/* a cdecl call of a routine in the C runtime replacement */
static uint32_t cdecl_call(uint32_t addr, int n, const uint32_t *a)
{
	X86_COUNT((uint64_t)n + 2); /* pushes, call, add esp */
	return hle_invoke(addr, n, a);
}

/* ---- the CD search (0x402590) ---- */

/* Look for the game CD: the first CD-ROM drive letter whose
 * SPEECH\A\A10930.WAV opens sets the prefix at CD_PREFIX; else the file is
 * tried in the current folder (prefix ""). Returns 1 if found. */
static uint32_t cd_search(void)
{
	uint32_t esp = cpu.r[ESP];
	cpu.r[ESP] -= 0x28;
	uint32_t local = cpu.r[ESP];
	X86_COUNT(6);
	uint32_t found = 0;
	for (uint32_t drive = 0; drive < 0x1a; drive++) {
		wr8(CD_PREFIX, 'A' + drive);
		X86_COUNT(5);
		if (IMPORT(IAT_GETDRIVETYPE, CD_PREFIX) != 5) {
			X86_COUNT(5);
			continue;
		}
		X86_COUNT(2);
		cdecl_call(0x412c90, 3, (const uint32_t[]){ local, CD_FMT, CD_PREFIX });
		X86_COUNT(2);
		uint32_t f = cdecl_call(0x412d13, 2, (const uint32_t[]){ local, MODE_READ });
		X86_COUNT(2);
		if (f) {
			found = f;
			break;
		}
		X86_COUNT(3);
	}
	if (!found) {
		wr8(CD_PREFIX, 0);
		found = cdecl_call(0x412d13, 2, (const uint32_t[]){ CD_FALLBACK_FILE, MODE_READ });
		X86_COUNT(3);
		if (!found) {
			X86_COUNT(5);
			cpu.r[ESP] = esp;
			return 0;
		}
	}
	cdecl_call(0x412d6e, 1, (const uint32_t[]){ found });
	X86_COUNT(7);
	cpu.r[ESP] = esp;
	return 1;
}

/* ---- display, sound and timer set-up (0x402620) ---- */

static void message_box(uint32_t text, uint32_t title)
{
	IMPORT(IAT_MESSAGEBOX, 0, text, title, 0x10);
}

/* Returns 1, or 0 after a message box when DirectDraw cannot be set up. */
static uint32_t display_init(void)
{
	uint32_t esp = cpu.r[ESP];
	cpu.r[ESP] -= 8; /* the colour key {8, 8} */
	uint32_t key = cpu.r[ESP];
	X86_COUNT(4);
	IMPORT(IAT_SHOWCURSOR, 1);
	/* DirectSound: the HLE 0x402ad0 (cdecl, the window) */
	cdecl_call(0x402ad0, 1, (const uint32_t[]){ SC(hwnd) });
	X86_COUNT(2);
	if (IMPORT(IAT_DDRAWCREATE, 0, DDRAW, 0) != 0) {
		message_box(MSG_DD_ERROR, MSG_TITLE_ERROR);
		cpu.r[ESP] = esp;
		X86_COUNT(5);
		return 0;
	}
	uint32_t dd = SC(ddraw);
	X86_COUNT(2);
	COM(dd, DD_SETCOOPERATIVELEVEL, SC(hwnd), 0x11);
	dd = SC(ddraw);
	if (COM(dd, DD_SETDISPLAYMODE, 640, 480, 8) != 0) {
		message_box(MSG_MODE_ERROR, MSG_TITLE_ERROR);
		cpu.r[ESP] = esp;
		X86_COUNT(5);
		return 0;
	}
	/* the screen: a primary surface */
	SC_SET(letterbox, 1);
	wr32(SURFACE_DESC, 0x6c);
	wr32(DESC_FLAGS, 1);
	wr32(DESC_CAPS, 0x200);
	X86_COUNT(5);
	dd = SC(ddraw);
	COM(dd, DD_CREATESURFACE, SURFACE_DESC, SURFACES, 0);
	uint32_t screen = rd32(SURFACES);
	COM1(screen, SURF_RESTORE);
	COM(screen, SURF_LOCK, 0, SURFACE_DESC, 1, 0);
	wr32(PITCHES, rd32(DESC_PITCH));
	COM(screen, SURF_UNLOCK, 0);
	X86_COUNT(3);
	/* transparent colour 8 */
	wr32(key, 8);
	wr32(key + 4, 8);
	COM(screen, SURF_SETCOLORKEY, 8, key);
	X86_COUNT(5);
	/* the palette: all black, shared by the screen */
	for (uint32_t i = 0; i < 256; i++)
		wr32(PALETTE_ENTRIES + 4 * i, 0);
	X86_COUNT(5 + 256);
	dd = SC(ddraw);
	COM(dd, DD_CREATEPALETTE, 0x44, PALETTE_ENTRIES, PALETTE_OBJ, 0);
	COM(rd32(SURFACES), SURF_SETPALETTE, SC(palette_obj));
	X86_COUNT(4);
	/* three off-screen surfaces 640 x 400 (system memory, colour key 8) */
	wr32(DESC_FLAGS, 7);
	wr32(DESC_CAPS, 0x840);
	wr32(DESC_WIDTH, 640);
	wr32(DESC_HEIGHT, 400);
	X86_COUNT(5);
	for (uint32_t i = 0; i < 3; i++) {
		dd = SC(ddraw);
		COM(dd, DD_CREATESURFACE, SURFACE_DESC, SURFACES + 4 + 4 * i, 0);
		COM(rd32(SURFACES + 4 + 4 * i), SURF_SETCOLORKEY, 8, key);
		wr32(PITCHES + 4 + 4 * i, 640);
		X86_COUNT(8);
	}
	/* the 640 x 480 scratch surface the text is drawn into */
	wr32(DESC_HEIGHT, 480);
	dd = SC(ddraw);
	COM(dd, DD_CREATESURFACE, SURFACE_DESC, SCRATCH_SURFACE, 0);
	X86_COUNT(6);
	/* fonts by size: 15, 24 and 32 pixels, Big5 */
	static const struct { int32_t height, weight; } fonts[3] = { { -15, 700 }, { -24, 700 }, { -32, 400 } };
	for (int i = 0; i < 3; i++) {
		uint32_t f = IMPORT(0x41b018, (uint32_t)fonts[i].height, 0, 0, 0, (uint32_t)fonts[i].weight, 0, 0, 0,
		                    0x88, 0, 0, 0, 0, FONT_FACE);
		wr32(FONTS + 4 * (uint32_t)i, f);
		X86_COUNT(7);
	}
	/* the 16 ms timer: the callback 0x4028c0 is the HLE */
	SC_SET(timer_id, IMPORT(IAT_TIMESETEVENT, 0x10, 0x10, 0x4028c0, 0, 1));
	IMPORT(IAT_SHOWCURSOR, 0);
	X86_COUNT(8);
	cpu.r[ESP] = esp;
	return 1;
}

/* ---- shutdown (0x402a20) ---- */

static void release_at(uint32_t slot)
{
	uint32_t obj = rd32(slot);
	X86_COUNT(2);
	if (obj) {
		COM1(obj, DD_RELEASE);
		wr32(slot, 0);
		X86_COUNT(2);
	}
}

static void shutdown(void)
{
	X86_COUNT(2);
	if (SC(timer_id))
		IMPORT(IAT_TIMEKILLEVENT, SC(timer_id));
	for (int i = 2; i >= 0; i--)
		IMPORT(IAT_DELETEOBJECT, rd32(FONTS + 4 * (uint32_t)i));
	X86_COUNT(5);
	release_at(PALETTE_OBJ);
	release_at(SCRATCH_SURFACE);
	for (uint32_t i = 0; i < 4; i++) {
		release_at(SURFACES + 4 * i);
		X86_COUNT(3);
	}
	release_at(DDRAW);
	/* DirectSound: the HLE 0x402c70 */
	X86_COUNT(1);
	HLE0(0x402c70);
	IMPORT(IAT_SHOWCURSOR, 1);
	X86_COUNT(1);
}

/* ---- WinMain (0x402090) ---- */

/* 0x402090: register the window class, create the window, find the CD,
 * play the logo (HLE), set the display up, show the window, run the script
 * VM (0x4060d4) and shut down when it ends. Returns WinMain's result: -1 the
 * class could not be registered, 0 no CD or no display, 1 the game ran.
 * The emulated stack keeps the native frame (4 argument and return words, the
 * 40-byte WNDCLASS, the saved EDI): the VM records its stack top
 * (RT.top_esp, part of the data area) from it. */
uint32_t game_main(uint32_t hinst)
{
	uint32_t esp0 = cpu.r[ESP];
	uint32_t wc = esp0 - 8 - 0x28;
	cpu.r[ESP] = wc - 4;
	X86_COUNT(5);
	SC_SET(hinstance, hinst);
	/* the class: style 0, window procedure 0x4021c0, no extra bytes */
	wr32(wc + 0, 0);
	wr32(wc + 4, 0x4021c0);
	wr32(wc + 8, 0);
	wr32(wc + 12, 0);
	wr32(wc + 16, hinst);
	wr32(wc + 20, IMPORT(IAT_LOADICON, hinst, ICON_NAME));
	wr32(wc + 24, IMPORT(IAT_LOADCURSOR, 0, 0x7f00));
	wr32(wc + 28, IMPORT(IAT_GETSTOCKOBJECT, 4));
	wr32(wc + 32, 0);
	wr32(wc + 36, WND_CLASS);
	X86_COUNT(12);
	uint32_t result;
	if ((IMPORT(IAT_REGISTERCLASS, wc) & 0xffff) == 0) {
		X86_COUNT(3);
		result = 0xffffffffu;
		goto leave;
	}
	X86_COUNT(10);
	uint32_t height = IMPORT(IAT_GETSYSMETRICS, 1);
	uint32_t width = IMPORT(IAT_GETSYSMETRICS, 0);
	wr32(HWND_MAIN, IMPORT(IAT_CREATEWINDOW, 0, WND_CLASS, WND_CLASS, 0x80000000u, 0, 0, width, height, 0, 0,
	                       SC(hinstance), 0));
	X86_COUNT(3);
	if (!cd_search()) {
		message_box(MSG_NO_CD, MSG_NO_CD_TITLE);
		X86_COUNT(3);
		result = 0;
		goto leave;
	}
	X86_COUNT(3);
	cdecl_call(0x401e20, 1, (const uint32_t[]){ LOGO_ARG }); /* the logo player (HLE) */
	uint32_t ok = display_init();
	X86_COUNT(2);
	if (!ok) {
		X86_COUNT(1);
		result = 0;
		goto leave;
	}
	IMPORT(IAT_SHOWWINDOW, SC(hwnd), 5);
	IMPORT(IAT_UPDATEWINDOW, SC(hwnd));
	X86_COUNT(2);
	X86_COUNT(1); /* call */
	game_vm_run();
	shutdown();
	X86_COUNT(1);
	result = 1;
leave:
	X86_COUNT(4); /* pop edi, add esp, ret 0x10 */
	cpu.r[ESP] = esp0;
	return result;
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
	X86_COUNT(1);
	HLE0(0x403440);
}

/* 0x402570: after the pointer moved with the cursor drawn: wait for the
 * vertical blank, redraw the cursor (restore the background, draw it
 * again). The cursor routines are native until the screen module is C. */
static void paint(void)
{
	uint32_t dd = SC(ddraw);
	COM(dd, DD_WAITVBLANK, 1, 0);
	X86_COUNT(2);
	game_cursor_restore();
	X86_COUNT(2);
	game_cursor_draw();
}

static void host_call(uint32_t slot, int n, const uint32_t *a)
{
	stdcall(rd32(slot), n, a);
}

/* The window procedure, stdcall (hwnd, msg, wparam, lparam): returns the
 * result for the message. Called by the HLE message pump (win32.c), which
 * keeps the CPU registers across it as x86_call did. The native registers
 * it leaves (ECX per path, EDX = 0) were never read. */
uint32_t game_wndproc(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
	uint32_t result = 0;
	X86_COUNT(5);
	if (msg == 0x200) { /* WM_MOUSEMOVE */
		uint32_t x = lparam & 0xffff, y = lparam >> 16;
		SC_SET(cursor_x, x);
		SV_SET(mouse_x, x);
		SC_SET(cursor_y, y);
		X86_COUNT(15);
		if (SC(letterbox)) {
			y = (y - 0x28) & 0xffff;
			X86_COUNT(4);
			if ((int16_t)y < 0) {
				y = 0;
				X86_COUNT(2);
			} else if ((int16_t)y > 0x18f) {
				y = 0x18f;
				X86_COUNT(3);
			} else {
				X86_COUNT(2);
			}
		}
		SV_SET(mouse_y, y);
		X86_COUNT(2);
		if (SC(cursor_on)) {
			X86_COUNT(1);
			paint();
		}
		goto done;
	}
	if (msg > 0x200) { /* mouse buttons */
		X86_COUNT(5);
		switch (msg) {
		case 0x201: /* left button down: stop the voice, OK pressed */
		case 0x204: { /* right button down: cancel pressed */
			voice_stop();
			RT_SET(input_bits, RT(input_bits) | (msg == 0x201 ? 0x10 : 0x20));
			X86_COUNT(4);
			goto done;
		}
		case 0x202:
		case 0x205:
			RT_SET(input_bits, RT(input_bits) & ~(msg == 0x202 ? 0x10 : 0x20));
			X86_COUNT(4);
			goto done;
		}
		goto def;
	}
	if (msg == 0x100) { /* WM_KEYDOWN */
		X86_COUNT(5);
		if (wparam - 0xd > 0x63)
			goto done;
		if (wparam == 0x70) { /* F1 */
			X86_COUNT(5);
			host_call(IAT_DESTROYWINDOW, 1, (const uint32_t[]){ SC(hwnd) });
			goto done;
		}
		uint32_t bit = key_bit(wparam);
		X86_COUNT(5);
		if (bit)
			RT_SET(input_bits, RT(input_bits) | bit);
		goto done;
	}
	if (msg > 0x100) {
		X86_COUNT(3);
		if (msg != 0x101)
			goto def;
		/* WM_KEYUP */
		if (wparam - 0xd > 0x20)
			goto done;
		uint32_t bit = key_bit(wparam);
		X86_COUNT(8);
		if (bit == 0x10 || bit == 0x20)
			voice_stop(); /* Enter, Space, Escape, Insert */
		if (bit)
			RT_SET(input_bits, RT(input_bits) & ~bit);
		goto done;
	}
	X86_COUNT(3);
	if (msg == 2) { /* WM_DESTROY */
		host_call(IAT_POSTQUIT, 1, (const uint32_t[]){ 0 });
		X86_COUNT(2);
		goto done;
	}
	if (msg != 0x1c) /* WM_ACTIVATEAPP */
		goto def;
	if (wparam == 0) { /* deactivated: pause the sound, stop the game's clock */
		SC_SET(paused, 1);
		SC_SET(active, 0);
		X86_COUNT(5);
		voice_stop();
		X86_COUNT(1);
		HLE0(0x403220); /* music pause (HLE) */
		goto done;
	}
	X86_COUNT(3);
	if (SC(paused)) {
		X86_COUNT(1);
		HLE0(0x403250); /* music resume (HLE) */
	}
	host_call(IAT_SETFOCUS, 1, (const uint32_t[]){ hwnd });
	SC_SET(active, 1);
	X86_COUNT(5);
	goto done;
def:
	X86_COUNT(5);
	result = stdcall(rd32(IAT_DEFWINDOWPROC), 4, (const uint32_t[]){ hwnd, msg, wparam, lparam });
done:
	X86_COUNT(3);
	return result;
}
