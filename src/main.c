/*
 * 同級生２ (nanpa2.exe, Taiwan release) on SDL2.
 *
 * Current milestone: image viewer for the game's .GP4 pictures, drawn into
 * the 640x480 8-bit screen the way the native game places its 640x400
 * content (40 lines down, native DAT_004ac068).
 *
 *   nanpa2 [--game DIR] [--view NAME.GP4] [--shot OUT.bmp]
 *   Left/Right: previous/next picture. Esc: quit.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fsys.h"
#include "gp4.h"
#include "platform/present.h"

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

int main(int argc, char **argv)
{
	const char *game = "game", *view = NULL, *shot = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--game") && i + 1 < argc)
			game = argv[++i];
		else if (!strcmp(argv[i], "--view") && i + 1 < argc)
			view = argv[++i];
		else if (!strcmp(argv[i], "--shot") && i + 1 < argc)
			shot = argv[++i];
	}
	fsys_set_root(game);
	list_gp4();

	int cur = 0;
	if (view) {
		for (int i = 0; i < name_count; i++)
			if (!stricmp(names[i], view))
				cur = i;
	}
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

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Window *win = SDL_CreateWindow("同級生２", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
					   SCREEN_W, SCREEN_H,
					   SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
						   present_window_flags());
	if (!win || !present_init(win)) {
		fprintf(stderr, "video init failed: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Log("Renderer: %s", present_name());

	for (int running = 1; running;) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_QUIT)
				running = 0;
			else if (ev.type == SDL_KEYDOWN) {
				SDL_Keycode k = ev.key.keysym.sym;
				if (k == SDLK_ESCAPE)
					running = 0;
				else if (name_count && (k == SDLK_RIGHT || k == SDLK_LEFT)) {
					cur = (cur + (k == SDLK_RIGHT ? 1 : name_count - 1)) % name_count;
					show(names[cur]);
				}
			}
		}
		present_frame(screen, palette);
		SDL_Delay(16);
	}
	present_shutdown();
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
