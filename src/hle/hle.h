/*
 * Hand-written replacements ("HLE") for everything the lifted code calls
 * outside itself: Win32 imports, the C runtime, DirectDraw/DirectSound COM
 * methods, and lifted functions replaced on purpose (sound, timer, pump).
 *
 * Calling convention: hle_call(addr) is entered with the native return
 * address on top of the emulated stack, as after an x86 `call`. It pops the
 * return address, runs, leaves the result in EAX, and pops the arguments
 * itself for stdcall functions. The caller continues at the return address.
 */
#ifndef NANPA2_HLE_H
#define NANPA2_HLE_H

#include <stddef.h>
#include <stdint.h>

#include "x86/x86.h"

/* Thunk addresses: imports at THUNK_IMPORT + 16*i, COM methods at
 * THUNK_COM + 0x100*interface + 4*method. Never valid memory. */
#define THUNK_IMPORT 0xf0000000u
#define THUNK_COM 0xf1000000u
#define THUNK_END 0xf2000000u

void hle_call(uint32_t addr);
void hle_trace_pump(void); /* NANPA2_HLETRACE line of the message pump, called directly by ctl_pump */

/* After hle_call popped the return address: argument i (0-based). */
static inline uint32_t hle_arg(int i)
{
	return rd32(cpu.r[ESP] + 4u * (uint32_t)i);
}

/* Pop n stdcall arguments. */
static inline void hle_stdcall_pop(int n)
{
	cpu.r[ESP] += 4u * (uint32_t)n;
}

/* Copy a NUL-terminated string from emulated memory (truncates). */
void hle_strcpy_in(char *dst, uint32_t src, size_t cap);
uint32_t hle_strlen(uint32_t a);

/* Dev probes (win32.c): the script VM fetched the opcode at offset off of
 * segment 0; the hit test (04 23) is about to scan the rectangle table at
 * script address table. They feed NANPA2_VMTRACE, the exploration bot and
 * NANPA2_HOTSPOTS. */
void probe_vm_fetch(uint32_t off);
void probe_hit_table(uint32_t table);

/* Setup: map imports (IAT) to thunks. Needs x86_load_image() first. */
int hle_init(const char *game_dir, const char *save_dir);

/* ---- modules ---- */

/* win32.c: window, messages, timer, files */
void win32_init(void);
uint32_t win32_import(const char *dll, const char *name, int *nargs, int *known);
int win32_call_import(int id);
void win32_post_message(uint32_t msg, uint32_t wparam, uint32_t lparam);
/* the same for a message from outside the game (host, scripted input, the bot):
 * recorded by --record, dropped while replaying (replay.h) */
void win32_post_ext(uint32_t msg, uint32_t wparam, uint32_t lparam);
uint32_t win32_timer_ticks(void); /* timer callbacks run so far */
int win32_pump(void);        /* native 0x401bf0: 0 on quit */
void win32_run_timer(void);  /* run due 16 ms ticks (native timer thread) */
int win32_timer_running(void); /* timeSetEvent seen, not killed */
/* Cursor. The native game hides the system cursor (ShowCursor counter -1:
 * 0x401e27, 0x40262a, 0x4028ab) and draws its own sprite from
 * DATA/MOUSE.DAT; the host cursor is hidden only while the counter is
 * negative and the pointer is over the game image of the focused window. */
int win32_show_cursor(int show); /* native ShowCursor: the new count */
void win32_cursor_update(void);  /* re-check after mouse/window/fit changes */
void win32_cursor_state(int *count, int *shown, int *over_image);
extern uint32_t win32_wndproc;

/* ddraw.c: DirectDraw */
uint32_t ddraw_create(void);
int ddraw_com_call(uint32_t iface, uint32_t method);
void ddraw_present(int force);
/* the primary surface as 640x480 palette indices and the palette as 0x00RRGGBB; 0 if none */
int ddraw_snapshot(uint8_t *frame, uint32_t pal[256]);
void ddraw_set_entries(uint32_t start, uint32_t count, uint32_t entries);
int ddraw_surface_info(uint32_t hdc, uint32_t *pixels, int *pitch, int *w, int *h);

/* text.c: GDI text into surfaces */
int text_init(const char *font_path);
uint32_t text_create_font(int height, int weight);
void text_out(uint32_t hdc, int x, int y, uint32_t str, int len);
void text_select(uint32_t hdc, uint32_t font);
void text_set_color(uint32_t hdc, uint32_t fg, uint32_t bg, int mode);

/* sound.c: see platform/sound.h */
#include "platform/sound.h"

/* Interface ids for COM thunks */
enum {
	IF_DDRAW = 1,
	IF_DDSURFACE,
	IF_DDPALETTE,
	IF_DSOUND,
	IF_DSBUFFER,
};
uint32_t com_vtable(int iface, int nmethods);
uint32_t com_new(int iface, int nmethods, uint32_t extra_bytes);

#endif
