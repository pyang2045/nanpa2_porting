/* Presenter for non-Apple platforms: SDL_Renderer with a streaming texture. */
#include "present_common.h"

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static uint32_t rgba[SCREEN_W * SCREEN_H];

Uint32 present_window_flags(void)
{
	return 0;
}

int present_init(SDL_Window *win)
{
	window = win;
	renderer = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!renderer)
		renderer = SDL_CreateRenderer(win, -1, 0);
	if (!renderer)
		return 0;
	SDL_RenderSetLogicalSize(renderer, SCREEN_W, SCREEN_H);
	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
				    SCREEN_W, SCREEN_H);
	return texture != NULL;
}

void present_frame(const uint8_t *pixels, const uint32_t pal[256])
{
	for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
		rgba[i] = pal[pixels[i]];
	SDL_UpdateTexture(texture, NULL, rgba, SCREEN_W * 4);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, texture, NULL, NULL);
	SDL_RenderPresent(renderer);
}

const char *present_name(void)
{
	static char name[64];
	SDL_RendererInfo info;
	if (renderer && !SDL_GetRendererInfo(renderer, &info))
		SDL_snprintf(name, sizeof name, "SDL_Renderer (%s)", info.name);
	return name;
}

int present_window_to_screen(int wx, int wy, int *sx, int *sy)
{
	return present_map_point(window, wx, wy, sx, sy);
}

void present_shutdown(void)
{
	if (texture)
		SDL_DestroyTexture(texture);
	if (renderer)
		SDL_DestroyRenderer(renderer);
	texture = NULL;
	renderer = NULL;
}
