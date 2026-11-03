/* Presenter for non-Apple platforms: SDL_Renderer with a streaming texture.
 * The CRT simulation is reduced to scanlines (a modulating overlay); there is
 * no curvature, so mouse mapping stays linear. */
#include "present_common.h"

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_Texture *scanlines;
static uint32_t rgba[SCREEN_W * SCREEN_H];
static int crt = 1;

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
	if (!texture)
		return 0;
	/* two overlay rows per screen line: full, then dimmed */
	scanlines = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, 1,
				      SCREEN_H * 2);
	if (scanlines) {
		static uint32_t rows[SCREEN_H * 2];
		for (int i = 0; i < SCREEN_H * 2; i++)
			rows[i] = i & 1 ? 0x00a0a0a0u : 0x00ffffffu;
		SDL_UpdateTexture(scanlines, NULL, rows, 4);
		SDL_SetTextureBlendMode(scanlines, SDL_BLENDMODE_MOD);
		SDL_SetTextureScaleMode(scanlines, SDL_ScaleModeLinear);
	}
	return 1;
}

void present_frame(const uint8_t *pixels, const uint32_t pal[256])
{
	for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
		rgba[i] = pal[pixels[i]];
	SDL_UpdateTexture(texture, NULL, rgba, SCREEN_W * 4);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, texture, NULL, NULL);
	if (crt && scanlines)
		SDL_RenderCopy(renderer, scanlines, NULL, NULL);
	SDL_RenderPresent(renderer);
}

int present_capture(const char *bmp_path, int w, int h)
{
	(void)w;
	(void)h; /* captures at the current output size */
	int ow, oh;
	if (SDL_GetRendererOutputSize(renderer, &ow, &oh))
		return 0;
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!s)
		return 0;
	int ok = !SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) &&
		 !SDL_SaveBMP(s, bmp_path);
	SDL_FreeSurface(s);
	return ok;
}

void present_set_crt(int on)
{
	crt = on;
}

int present_get_crt(void)
{
	return crt;
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
	return present_map_point(window, wx, wy, 0, sx, sy);
}

void present_shutdown(void)
{
	if (scanlines)
		SDL_DestroyTexture(scanlines);
	if (texture)
		SDL_DestroyTexture(texture);
	scanlines = NULL;
	if (renderer)
		SDL_DestroyRenderer(renderer);
	texture = NULL;
	renderer = NULL;
}
