/*
 * 同級生２ (nanpa2.exe, Taiwan release) on SDL2.
 *
 *   nanpa2 [--game DIR] [--font PATH]                  play
 *   nanpa2 --view [NAME.GP4] [--shot OUT.bmp]          picture viewer
 *   nanpa2 --view NAME.GP4 --crt-shot OUT.bmp          presenter output, 1280x960
 *          [--crt-size WxH]                             ... at another size
 *                                                       (0x0: the window's drawable)
 *   nanpa2 --view-bmp SHOT.bmp --crt-shot OUT.bmp      the same for a 640x480
 *                                                       screenshot (NANPA2_SHOT_DIR)
 *   --speed X (or NANPA2_SPEED=X)                       game speed, 1.0 = the original's, 0 = as fast as
 *                                                       the host can (virtual clock)
 *   --display hd|480|sharp|smooth                       CRT HD (default), CRT 480, Sharp, Smooth
 *                                                       (F12 cycles them; remembered in display.txt
 *                                                       of the save folder for plain launches)
 *   --no-crt                                            = --display sharp
 *   --text hires|bitmap                                 vector text at the display's resolution over the
 *                                                       picture (default), or the game's own bitmap
 *                                                       glyphs (NANPA2_TEXT_LAYER; F11 toggles, remembered
 *                                                       in display.txt; platform/textlayer.h)
 *   --crt-lines N                                       CRT HD's virtual raster height (600..1600,
 *                                                       default 1080; NANPA2_CRT_LINES)
 *   --crt-scaler sharp|smooth                           CRT HD's scaler (NANPA2_CRT_SCALER)
 *   nanpa2 --logo-frame N --shot OUT.bmp               decode LOGO.AVI frame N
 *   --logo (or NANPA2_LOGO=1)                           play the LOGO.AVI publisher
 *                                                       movie (skipped by default;
 *                                                       --no-logo is accepted)
 *   --window WxH                                        initial window size (points)
 *   --fullscreen                                        start fullscreen
 *                                                       (Alt+Enter, Cmd+F toggle)
 *   --record FILE / --replay FILE                       record the play (inputs and state
 *                                                       checkpoints) / replay it and
 *                                                       compare (src/dev/replay.h)
 *   --deterministic                                     the game clock is the virtual time
 *                                                       of the pacing model, not the host's
 *   --pause-inactive                                    stop the game while the
 *                                                       window is in the background
 *                                                       (the original behaviour)
 *
 * Without --game the game folder is looked up (launch.c): NANPA2_GAME_DIR,
 * the remembered choice, next to the executable or .app, ./game, then a
 * folder picker.
 *
 * Playing runs the game's C reimplementation (src/game) on the platform API
 * (src/platform: files, surfaces, text, window, sound). The game state starts from
 * values defined in code (game/state.c): neither nanpa2.exe nor nanpa2.dat is read.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fsys.h"
#include "gp4.h"
#include "platform/present.h"
#include "platform/clock.h"
#include "dev/dev.h"
#include "platform/files.h"
#include "platform/logo.h"
#include "platform/sound.h"
#include "platform/text.h"
#include "game/startup.h"
#include "game/state.h"
#include "dev/replay.h"
#include "dev/status.h"
#include "launch.h"

#ifndef _WIN32
#include <dirent.h>
#include <strings.h>
#define stricmp strcasecmp
#else
#include <io.h>
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

static void add_name(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot || stricmp(dot, ".GP4"))
		return;
	names = realloc(names, sizeof *names * (name_count + 1));
	names[name_count++] = strdup(name);
}

static void list_gp4(void)
{
#ifdef _WIN32
	char path[1100];
	snprintf(path, sizeof path, "%s\\DATA\\*.GP4", fsys_root());
	struct _finddata_t e;
	intptr_t h = _findfirst(path, &e);
	if (h == -1)
		return;
	do
		add_name(e.name);
	while (_findnext(h, &e) == 0);
	_findclose(h);
#else
	char path[1100];
	snprintf(path, sizeof path, "%s/DATA", fsys_root());
	DIR *d = opendir(path);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d)))
		add_name(e->d_name);
	closedir(d);
#endif
	qsort(names, name_count, sizeof *names, cmp_name);
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

/* the game speed: 1.0 is the original's (the time model of game/pace.h at real time) */
static double game_speed = 1.0;
static int game_speed_set;

static SDL_Window *game_window;

static int run_game(const char *game, const char *font)
{
	state_init();
	/* NANPA2_SAVE_DIR: FLAG files elsewhere than the pref folder (tests) */
	const char *save_env = getenv("NANPA2_SAVE_DIR");
	char *save = save_env ? NULL : SDL_GetPrefPath("nanpa2", "nanpa2");
	files_init(game, save_env ? save_env : save ? save : ".");
	text_init(font);
	dev_init();
	replay_init(save_env ? save_env : save ? save : ".");
	status_init(game_window, game_speed);
	clock_init(game_speed);
	if (getenv("NANPA2_SAVETEST"))
		return dev_save_selftest();
	if (replay_replaying() && !game_speed_set)
		clock_init(0); /* a replay runs as fast as the host can */
	/* native WinMain body (0x402090): window, CD check, DirectDraw, then the
	 * script VM until the game ends */
	uint64_t t0 = SDL_GetPerformanceCounter();
	game_main();
	replay_finish();
	SDL_free(save);
	/* with NANPA2_SPEED=0: the game time against the host's */
	double secs = (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency();
	double game_secs = (double)clock_us() / 1e6;
	SDL_Log("Game: %.1f s of game time in %.1f s (%.2fx)", game_secs, secs, secs > 0 ? game_secs / secs : 0.0);
	return 0;
}

/* The display mode: --display, else (plain launches only, not scripted, headless or shot runs) the one
 * remembered in <save folder>/display.txt, else CRT HD; later changes by F12 are remembered. */
static int pref_on;

static void pref_path(char *out, size_t cap)
{
	snprintf(out, cap, "%s/display.txt", launch_pref_dir() ? launch_pref_dir() : ".");
}

/* display.txt: the display mode on the first line, the text layer (hires or bitmap) on the second */
static void pref_save(int mode)
{
	(void)mode;
	char p[1100];
	pref_path(p, sizeof p);
	FILE *f = fopen(p, "wb");
	if (f) {
		int m = present_get_mode();
		fprintf(f, "%s\ntext %s\n", m == DISPLAY_CRT_HD ? "hd" : m == DISPLAY_CRT_480 ? "480" :
						 m == DISPLAY_SHARP ? "sharp" : "smooth",
			tl_mode_name(tl_get_mode()));
		fclose(f);
	}
}

static void display_setup(int display, int text, int use_pref)
{
	const char *e = getenv("NANPA2_DISPLAY");
	if (display < 0 && e)
		display = present_parse_mode(e);
	pref_on = use_pref || getenv("NANPA2_DISPLAY_PREF");
	if (text < 0 && getenv("NANPA2_TEXT_LAYER"))
		text = tl_parse_mode(getenv("NANPA2_TEXT_LAYER"));
	if ((display < 0 || text < 0) && pref_on) {
		char p[1100], line[32] = "";
		pref_path(p, sizeof p);
		FILE *f = fopen(p, "rb");
		if (f) {
			if (fgets(line, sizeof line, f)) {
				line[strcspn(line, "\r\n")] = 0;
				if (display < 0)
					display = present_parse_mode(line);
			}
			if (fgets(line, sizeof line, f) && !strncmp(line, "text ", 5)) {
				line[strcspn(line, "\r\n")] = 0;
				if (text < 0)
					text = tl_parse_mode(line + 5);
			}
			fclose(f);
		}
	}
	tl_set_mode(text < 0 ? TEXT_HIRES : text);
	present_set_mode(display < 0 ? DISPLAY_CRT_HD : display);
	if (getenv("NANPA2_CRT_LINES"))
		present_set_crt_lines(atoi(getenv("NANPA2_CRT_LINES")));
	if (getenv("NANPA2_CRT_SCALER"))
		present_set_crt_scaler(!strcmp(getenv("NANPA2_CRT_SCALER"), "smooth"));
	if (pref_on) {
		present_on_mode_change(pref_save);
		tl_on_change(pref_save);
	}
}

int main(int argc, char **argv)
{
	const char *game = NULL, *view = NULL, *shot = NULL, *font = NULL;
	const char *crt_shot = NULL, *view_bmp = NULL;
	int win_w = SCREEN_W, win_h = SCREEN_H;
	int viewer = 0, display = -1, text = -1, fullscreen = 0, logo_frame_n = -1, crt_w = SCREEN_W * 2, crt_h = SCREEN_H * 2;
	double speed = 1.0;
	int speed_set = 0;
	dev_install_hooks();
	if (argc > 2 && !strcmp(argv[1], "--wav-check"))
		return sound_wav_check(argc - 2, argv + 2) ? 1 : 0;
	launch_log_init(argc, argv);
	/* NANPA2_TURBO=N is the old name of NANPA2_SPEED=N */
	if (getenv("NANPA2_SPEED"))
		speed = atof(getenv("NANPA2_SPEED")), speed_set = 1;
	else if (getenv("NANPA2_TURBO") && atoi(getenv("NANPA2_TURBO")) > 1)
		speed = atoi(getenv("NANPA2_TURBO")), speed_set = 1;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--game") && i + 1 < argc)
			game = argv[++i];
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
			display = DISPLAY_SHARP;
		else if (!strcmp(argv[i], "--display") && i + 1 < argc) {
			display = present_parse_mode(argv[++i]);
			if (display < 0)
				fprintf(stderr, "--display: hd, 480, sharp or smooth\n");
		} else if (!strcmp(argv[i], "--text") && i + 1 < argc) {
			text = tl_parse_mode(argv[++i]);
			if (text < 0)
				fprintf(stderr, "--text: hires or bitmap\n");
		} else if (!strcmp(argv[i], "--crt-lines") && i + 1 < argc)
			SDL_setenv("NANPA2_CRT_LINES", argv[++i], 1);
		else if (!strcmp(argv[i], "--crt-scaler") && i + 1 < argc)
			SDL_setenv("NANPA2_CRT_SCALER", argv[++i], 1);
		else if (!strcmp(argv[i], "--logo-frame") && i + 1 < argc)
			logo_frame_n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--logo"))
			SDL_setenv("NANPA2_LOGO", "1", 1);
		else if (!strcmp(argv[i], "--no-logo"))
			SDL_setenv("NANPA2_LOGO", "0", 1);
		else if (!strcmp(argv[i], "--window") && i + 1 < argc)
			sscanf(argv[++i], "%dx%d", &win_w, &win_h);
		else if (!strcmp(argv[i], "--fullscreen"))
			fullscreen = 1;
		else if (!strcmp(argv[i], "--speed") && i + 1 < argc)
			speed = atof(argv[++i]), speed_set = 1;
		else if (!strcmp(argv[i], "--record") && i + 1 < argc)
			SDL_setenv("NANPA2_RECORD", argv[++i], 1);
		else if (!strcmp(argv[i], "--replay") && i + 1 < argc)
			SDL_setenv("NANPA2_REPLAY", argv[++i], 1);
		else if (!strcmp(argv[i], "--deterministic"))
			SDL_setenv("NANPA2_DETERMINISTIC", "1", 1);
		else if (!strcmp(argv[i], "--pause-inactive"))
			SDL_setenv("NANPA2_PAUSE_INACTIVE", "1", 1);
	}
	/* the game folder: --game, or found or asked for (launch.c); message
	 * boxes and the picker only for a launch someone watches */
	if (!view_bmp) {
		int interactive = !getenv("NANPA2_HEADLESS") && !getenv("NANPA2_INPUT") && !shot && !crt_shot;
		game = launch_game_dir(game, interactive);
		if (!game)
			return 1;
	} else if (!game)
		game = "game";
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

	/* NANPA2_HEADLESS (scripted and smoke test runs): SDL's dummy video
	 * driver and the presenter's software path, no window server or GPU, so
	 * test runs leave nothing on the desktop and also run while the screen
	 * is locked (where the display and GPU setup can block for good) */
	int headless = getenv("NANPA2_HEADLESS") != NULL;
	if (headless) {
		SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
		SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
	}
	/* the status file exists from here and names the startup stage: the
	 * video setup can hang with the screen locked, see status.c */
	if (!viewer)
		status_early();
	status_stage("sdl_init");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) < 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	status_stage("window");
	SDL_Window *win = SDL_CreateWindow("同級生２", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
					   win_w, win_h,
					   SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
						   (headless ? SDL_WINDOW_HIDDEN : 0) | present_window_flags());
	status_stage("present_init");
	if (!win || !present_init(win)) {
		fprintf(stderr, "video init failed: %s\n", SDL_GetError());
		return 1;
	}
	status_stage("game_init");
	SDL_Log("Renderer: %s", present_name());
	/* A binary started from a terminal is not activated by macOS 14+, so its
	 * window can open behind the terminal; bring it to the front. */
	if (!headless)
		SDL_RaiseWindow(win);
	display_setup(display, text, !headless && !getenv("NANPA2_INPUT") && !shot && !crt_shot &&
				      !getenv("NANPA2_REPLAY") && !getenv("NANPA2_RECORD") &&
				      !getenv("NANPA2_DETERMINISTIC"));
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
		game_speed = speed;
		game_speed_set = speed_set;
		game_window = win;
		rc = run_game(game, font);
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
