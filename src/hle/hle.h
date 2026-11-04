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
int hle_is_target(uint32_t addr);
extern const uint32_t x86_hle_targets[]; /* from the generated code */

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

/* Setup: map imports (IAT) to thunks. Needs x86_load_image() first. */
int hle_init(const char *game_dir, const char *save_dir);

/* ---- modules ---- */

/* win32.c: window, messages, timer, files */
void win32_init(void);
uint32_t win32_import(const char *dll, const char *name, int *nargs, int *known);
int win32_call_import(int id);
void win32_post_message(uint32_t msg, uint32_t wparam, uint32_t lparam);
int win32_pump(void);        /* native 0x401bf0: 0 on quit */
void win32_run_timer(void);  /* run due 16 ms ticks (native timer thread) */
int win32_timer_running(void); /* timeSetEvent seen, not killed */
extern uint32_t win32_wndproc;

/* crt.c: C runtime replacements by native address */
int crt_call(uint32_t addr);
void crt_init(const char *game_dir, const char *save_dir);

/* ddraw.c: DirectDraw */
uint32_t ddraw_create(void);
int ddraw_com_call(uint32_t iface, uint32_t method);
void ddraw_present(int force);
void ddraw_set_entries(uint32_t start, uint32_t count, uint32_t entries);
int ddraw_surface_info(uint32_t hdc, uint32_t *pixels, int *pitch, int *w, int *h);

/* text.c: GDI text into surfaces */
int text_init(const char *font_path);
uint32_t text_create_font(int height, int weight);
void text_out(uint32_t hdc, int x, int y, uint32_t str, int len);
void text_select(uint32_t hdc, uint32_t font);
void text_set_color(uint32_t hdc, uint32_t fg, uint32_t bg, int mode);

/* sound.c: music and voices (replaces the native DirectSound/MP3 code) */
int sound_init(void);
void sound_shutdown(void);
void sound_music_play(int n);
void sound_music_stop(void);
int sound_status(char *voice_out, size_t cap); /* music number or -1 */
void sound_music_pause(int pause);
void sound_voice_play(const char *rel);
void sound_voice_stop(void);
int sound_wav_check(int n, char **files); /* --wav-check; returns failures */

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
