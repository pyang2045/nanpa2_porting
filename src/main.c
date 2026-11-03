/*
 * 同級生２ (nanpa2.exe, Taiwan release) on SDL2.
 *
 *   nanpa2 [--game DIR] [--exe PATH] [--font PATH]     play
 *   nanpa2 --view [NAME.GP4] [--shot OUT.bmp]          picture viewer
 *   nanpa2 --view NAME.GP4 --crt-shot OUT.bmp          presenter output, 1280x960
 *          [--crt-size WxH]                             ... at another size
 *                                                       (0x0: the window's drawable)
 *   nanpa2 --view-bmp SHOT.bmp --crt-shot OUT.bmp      the same for a 640x480
 *                                                       screenshot (NANPA2_SHOT_DIR)
 *   --cpu-mips N (or NANPA2_CPU_MIPS=N)                 emulated CPU speed, 0 = unthrottled
 *   --no-crt                                            plain pixels (F12 toggles)
 *   nanpa2 --logo-frame N --shot OUT.bmp               decode LOGO.AVI frame N
 *   --no-logo                                           skip LOGO.AVI
 *   --window WxH                                        initial window size (points)
 *   --fullscreen                                        start fullscreen
 *                                                       (Alt+Enter, Cmd+F toggle)
 *   --pause-inactive                                    stop the game while the
 *                                                       window is in the background
 *                                                       (the original behaviour)
 *
 * Playing runs the game code lifted from nanpa2.exe (tools/recomp.py) on the
 * hand-written Win32/DirectX layer in src/hle.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fsys.h"
#include "gp4.h"
#include "platform/present.h"
#include "hle/dev.h"
#include "hle/hle.h"
#include "hle/logo.h"
#include "hle/status.h"
#include "x86/x86.h"

#ifndef _WIN32
#include <dirent.h>
#include <strings.h>
#define stricmp strcasecmp
#else
#define stricmp _stricmp
#endif

#define CONTENT_Y 40 /* 640x400 content sits at y=40 in the 480-line screen */

static uint8_t screen[SCREEN_W * SCREEN_H];
static uint32_t palette[256];
static const uint32_t *logo_rgb; /* --logo-frame: show this RGB frame */

static char **names;
static int name_count;

static int cmp_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void list_gp4(void)
{
#ifndef _WIN32
	char path[1100];
	snprintf(path, sizeof path, "%s/DATA", fsys_root());
	DIR *d = opendir(path);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		const char *dot = strrchr(e->d_name, '.');
		if (!dot || stricmp(dot, ".GP4"))
			continue;
		names = realloc(names, sizeof *names * (name_count + 1));
		names[name_count++] = strdup(e->d_name);
	}
	closedir(d);
	qsort(names, name_count, sizeof *names, cmp_name);
#endif
}

static int show(const char *name)
{
	struct gp4 g;
	if (!gp4_load(name, &g)) {
		SDL_Log("cannot load %s", name);
		return 0;
	}
	memset(screen, 0, sizeof screen);
	for (int y = 0; y < g.h; y++) {
		int sy = g.y + CONTENT_Y + y;
		if (sy < 0 || sy >= SCREEN_H)
			continue;
		for (int x = 0; x < g.w; x++) {
			int sx = g.x + x;
			if (sx >= 0 && sx < SCREEN_W)
				screen[sy * SCREEN_W + sx] = g.pixels[y * g.w + x];
		}
	}
	gp4_palette_rgb(g.pal_raw, palette);
	SDL_Log("%s: %dx%d at (%d,%d)", name, g.w, g.h, g.x, g.y);
	gp4_free(&g);
	return 1;
}

/* Load a 640x480 screenshot (as dev.c writes them) as screen + palette;
 * the game screen has at most 256 colours. */
static int show_bmp(const char *path)
{
	SDL_Surface *b = SDL_LoadBMP(path);
	SDL_Surface *s = b ? SDL_ConvertSurfaceFormat(b, SDL_PIXELFORMAT_XRGB8888, 0) : NULL;
	SDL_FreeSurface(b);
	if (!s || s->w != SCREEN_W || s->h != SCREEN_H) {
		SDL_Log("cannot load %s as a %dx%d screenshot", path, SCREEN_W, SCREEN_H);
		SDL_FreeSurface(s);
		return 0;
	}
	int n = 0;
	for (int y = 0; y < SCREEN_H; y++) {
		const uint32_t *row = (const uint32_t *)((uint8_t *)s->pixels + y * s->pitch);
		for (int x = 0; x < SCREEN_W; x++) {
			uint32_t c = row[x] & 0xffffff;
			int i = 0;
			while (i < n && palette[i] != c)
				i++;
			if (i == n && n < 256)
				palette[n++] = c;
			screen[y * SCREEN_W + x] = (uint8_t)(i < 256 ? i : 0);
		}
	}
	SDL_FreeSurface(s);
	return 1;
}

static void show_screen(void)
{
	if (logo_rgb)
		present_frame_rgb(logo_rgb);
	else
		present_frame(screen, palette);
}

static void save_bmp(const char *path)
{
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_W, SCREEN_H, 32, SDL_PIXELFORMAT_XRGB8888);
	for (int y = 0; y < SCREEN_H; y++) {
		uint32_t *row = (uint32_t *)((uint8_t *)s->pixels + y * s->pitch);
		for (int x = 0; x < SCREEN_W; x++)
			row[x] = palette[screen[y * SCREEN_W + x]];
	}
	SDL_SaveBMP(s, path);
	SDL_FreeSurface(s);
}

/* 486DX2-66 class: 66 MHz at about 2.6 clocks per instruction for this
 * kind of integer code (memory operands, cache misses, Windows 95 around
 * it) gives about 25 million instructions per second. */
#define NANPA2_DEFAULT_MIPS 25.0
static double cpu_mips = NANPA2_DEFAULT_MIPS;

static SDL_Window *game_window;

static int run_game(const char *game, const char *exe, const char *font)
{
	char exe_buf[1100];
	if (!exe) {
		snprintf(exe_buf, sizeof exe_buf, "%s/nanpa2.exe", game);
		exe = exe_buf;
	}
	if (!x86_load_image(exe)) {
		fprintf(stderr, "cannot load %s\n", exe);
		return 1;
	}
	/* NANPA2_SAVE_DIR: FLAG files elsewhere than the pref folder (tests) */
	const char *save_env = getenv("NANPA2_SAVE_DIR");
	char *save = save_env ? NULL : SDL_GetPrefPath("nanpa2", "nanpa2");
	if (!hle_init(game, save_env ? save_env : save ? save : ".")) {
		fprintf(stderr, "cannot map imports of %s\n", exe);
		return 1;
	}
	SDL_free(save);
	text_init(font);
	dev_init();
	status_init(game_window, cpu_mips);
	x86_set_speed(cpu_mips * 1e6);
	if (getenv("NANPA2_SAVETEST"))
		return dev_save_selftest();
	/* native WinMain body (0x402090): window, CD check, DirectDraw, then the
	 * script interpreter until the game ends */
	uint32_t hinstance = MEM_BASE;
	x86_call(0x402090, 1, &hinstance);
	return 0;
}

int main(int argc, char **argv)
{
	const char *game = "game", *view = NULL, *shot = NULL, *exe = NULL, *font = NULL;
	const char *crt_shot = NULL, *view_bmp = NULL;
	int win_w = SCREEN_W, win_h = SCREEN_H;
	int viewer = 0, crt = 1, fullscreen = 0, logo_frame_n = -1, crt_w = SCREEN_W * 2, crt_h = SCREEN_H * 2;
	/* Emulated CPU speed in MIPS (0 = unthrottled), see x86_throttle() */
	double mips = NANPA2_DEFAULT_MIPS;
	if (getenv("NANPA2_CPU_MIPS"))
		mips = atof(getenv("NANPA2_CPU_MIPS"));
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--game") && i + 1 < argc)
			game = argv[++i];
		else if (!strcmp(argv[i], "--exe") && i + 1 < argc)
			exe = argv[++i];
		else if (!strcmp(argv[i], "--font") && i + 1 < argc)
			font = argv[++i];
		else if (!strcmp(argv[i], "--view")) {
			viewer = 1;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				view = argv[++i];
		} else if (!strcmp(argv[i], "--shot") && i + 1 < argc)
			shot = argv[++i];
		else if (!strcmp(argv[i], "--crt-shot") && i + 1 < argc)
			crt_shot = argv[++i];
		else if (!strcmp(argv[i], "--crt-size") && i + 1 < argc)
			sscanf(argv[++i], "%dx%d", &crt_w, &crt_h);
		else if (!strcmp(argv[i], "--view-bmp") && i + 1 < argc) {
			viewer = 1;
			view_bmp = argv[++i];
		} else if (!strcmp(argv[i], "--no-crt"))
			crt = 0;
		else if (!strcmp(argv[i], "--logo-frame") && i + 1 < argc)
			logo_frame_n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--no-logo"))
			SDL_setenv("NANPA2_LOGO", "0", 1);
		else if (!strcmp(argv[i], "--window") && i + 1 < argc)
			sscanf(argv[++i], "%dx%d", &win_w, &win_h);
		else if (!strcmp(argv[i], "--fullscreen"))
			fullscreen = 1;
		else if (!strcmp(argv[i], "--cpu-mips") && i + 1 < argc)
			mips = atof(argv[++i]);
		else if (!strcmp(argv[i], "--pause-inactive"))
			SDL_setenv("NANPA2_PAUSE_INACTIVE", "1", 1);
	}
	fsys_set_root(game);

	int cur = 0;
	if (logo_frame_n >= 0) {
		/* the decoded movie frame goes through the presenter as RGB */
		static uint32_t rgb[SCREEN_W * SCREEN_H];
		int n = logo_frame(logo_frame_n, rgb);
		if (!n)
			return 1;
		SDL_Log("LOGO.AVI frame %d of %d", logo_frame_n, n);
		if (shot) {
			SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(rgb, SCREEN_W, SCREEN_H, 32, SCREEN_W * 4,
									    SDL_PIXELFORMAT_XRGB8888);
			SDL_SaveBMP(s, shot);
			SDL_FreeSurface(s);
			return 0;
		}
		logo_rgb = rgb;
		viewer = 1;
	} else if (view_bmp) {
		if (SDL_Init(0) < 0 || !show_bmp(view_bmp))
			return 1;
	} else if (viewer) {
		list_gp4();
		if (view)
			for (int i = 0; i < name_count; i++)
				if (!stricmp(names[i], view))
					cur = i;
		if (!view && !name_count) {
			fprintf(stderr, "no DATA/*.GP4 under %s\n", game);
			return 1;
		}
		if (!show(view && !name_count ? view : names[cur]))
			return 1;
		if (shot) {
			save_bmp(shot);
			return 0;
		}
	}

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) < 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	/* NANPA2_HEADLESS (scripted test runs): the window and presenter exist,
	 * for present_capture, but the window is never shown, so test runs
	 * leave no black window on the desktop */
	int headless = getenv("NANPA2_HEADLESS") != NULL;
	SDL_Window *win = SDL_CreateWindow("同級生２", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
					   win_w, win_h,
					   SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
						   (headless ? SDL_WINDOW_HIDDEN : 0) | present_window_flags());
	if (!win || !present_init(win)) {
		fprintf(stderr, "video init failed: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Log("Renderer: %s", present_name());
	/* A binary started from a terminal is not activated by macOS 14+, so its
	 * window can open behind the terminal; bring it to the front. */
	if (!headless)
		SDL_RaiseWindow(win);
	present_set_crt(crt);
	if (fullscreen)
		present_set_fullscreen(1);
	if (viewer && crt_shot) {
		/* let the window settle (fullscreen transition) for drawable-size shots */
		for (Uint32 t = SDL_GetTicks() + (fullscreen ? 1500 : 100); SDL_GetTicks() < t;) {
			SDL_Event ev;
			while (SDL_PollEvent(&ev)) {
			}
			show_screen();
			SDL_Delay(16);
		}
		int ok = present_capture(crt_shot, crt_w, crt_h);
		present_shutdown();
		SDL_DestroyWindow(win);
		SDL_Quit();
		return ok ? 0 : 1;
	}

	int rc = 0;
	if (!viewer) {
		cpu_mips = mips;
		game_window = win;
		rc = run_game(game, exe, font);
		status_shutdown();
	} else {
		for (int running = 1; running;) {
			SDL_Event ev;
			while (SDL_PollEvent(&ev)) {
				if (ev.type == SDL_QUIT)
					running = 0;
				else if (ev.type == SDL_KEYDOWN && !present_hotkey(&ev.key)) {
					SDL_Keycode k = ev.key.keysym.sym;
					if (k == SDLK_ESCAPE)
						running = 0;
					else if (name_count && (k == SDLK_RIGHT || k == SDLK_LEFT)) {
						cur = (cur + (k == SDLK_RIGHT ? 1 : name_count - 1)) % name_count;
						show(names[cur]);
					}
				}
			}
			show_screen();
			SDL_Delay(16);
		}
	}
	present_shutdown();
	SDL_DestroyWindow(win);
	SDL_Quit();
	return rc;
}
