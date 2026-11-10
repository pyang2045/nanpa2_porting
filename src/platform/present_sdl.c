/* Presenter for non-Apple platforms (or -DNANPA2_SDL_RENDERER=ON): SDL_Renderer.
 *
 * The CRT simulation approximates the Metal shader's flat Sony FD Trinitron
 * with blended textures:
 *   1. the screen is copied (nearest) into a k-times larger render target,
 *      k = output scale rounded up (1..4), and scanlines are multiplied in
 *      there so they stay locked to the screen lines;
 *   2. that is drawn (linear) over the dark grey glass, a small blurred copy
 *      is added for the glow;
 *   3. an aperture-grille stripe texture (one texel per output pixel), the
 *      two damper wires and a mild vignette are multiplied in.
 * The face is flat, so mouse mapping is linear.
 *
 * Display modes (present.h): CRT 480 is the above. CRT HD does the same on a virtual raster of
 * present_set_crt_lines lines (the picture is first scaled by an integer factor, nearest, then linearly
 * down to the raster, so the scanlines are at the raster's pitch). Sharp is the integer-scaled copy
 * scaled linearly to the window (even pixels at integer scales, a soft seam elsewhere); Smooth is
 * the same. The Metal presenter does these with exact area averaging and Catmull-Rom.
 *
 * The high-resolution text layer (platform/textlayer.h) is not drawn here: the layer is tracked and
 * validated by the platform, but SDL_Renderer has no linear-light blending into the CRT's raster, so this
 * presenter never asks for it and shows the game's own glyphs (a documented no-op there). present_metal.m is the implementation. */
#include "present_common.h"

#define MAX_K 4

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;               /* the screen, 640x480 */
static SDL_Texture *big;                   /* the screen at k x, with scanlines */
static SDL_Texture *scan_tex[MAX_K + 1];   /* 1 x (k * lines) scanline overlays */
static int scan_lines[MAX_K + 1];
static SDL_Texture *ipix;                  /* the screen at an integer factor, nearest */
static int ipix_m, big_w, big_h;
static SDL_Texture *glow_half, *glow_quarter;
static SDL_Texture *vignette;
static SDL_Texture *grille;                /* grille_w x 1 stripes */
static int big_k, grille_w;
static float grille_ms;
static uint32_t rgba[SCREEN_W * SCREEN_H];

SDL_WindowFlags present_window_flags(void)
{
#ifdef SDL_PLATFORM_IOS
	return SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS;
#else
	return 0;
#endif
}

static char name[64];

static float smoothstep(float a, float b, float x)
{
	float t = (x - a) / (b - a);
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	return t * t * (3 - 2 * t);
}

static uint32_t grey(float v)
{
	uint32_t c = (uint32_t)(v * 255.0f + 0.5f);
	return c << 16 | c << 8 | c;
}

/* Scanline overlay for k output rows per screen line, 1 x (k * SCREEN_H)
 * (modulate blend): the rows away from the beam centre are the thin dark
 * gaps. Stronger with more rows, as in Metal. */
static SDL_Texture *make_scanlines(int k, int lines)
{
	SDL_Texture *t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, 1,
					   k * lines);
	if (!t)
		return NULL;
	static uint32_t rows[MAX_K * 1600];
	float depth = 0.35f * smoothstep(1.0f, 3.0f, (float)k);
	for (int i = 0; i < k; i++) {
		float d = ((i + 0.5f) / k - 0.5f) * 2.0f; /* from the beam centre */
		for (int line = 0; line < lines; line++)
			rows[line * k + i] = grey(1.0f - depth * d * d);
	}
	SDL_UpdateTexture(t, NULL, rows, 4);
	SDL_SetTextureBlendMode(t, SDL_BLENDMODE_MOD);
	SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
	return t;
}

static SDL_Texture *make_vignette(void)
{
	enum { VW = 64, VH = 48 };
	SDL_Texture *t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, VW, VH);
	if (!t)
		return NULL;
	static uint32_t px[VW * VH];
	for (int y = 0; y < VH; y++)
		for (int x = 0; x < VW; x++) {
			float u = (x + 0.5f) / VW, v = (y + 0.5f) / VH;
			px[y * VW + x] = grey(SDL_powf(16.0f * u * (1 - u) * v * (1 - v), 0.06f));
		}
	SDL_UpdateTexture(t, NULL, px, VW * 4);
	SDL_SetTextureBlendMode(t, SDL_BLENDMODE_MOD);
	SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
	return t;
}

/* Vertical R, G, B stripes, one texel per output pixel, strength ms. */
static void make_grille(int w, float ms)
{
	if (grille && grille_w == w && grille_ms == ms)
		return;
	if (grille)
		SDL_DestroyTexture(grille);
	grille = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, w, 1);
	grille_w = w;
	grille_ms = ms;
	if (!grille)
		return;
	uint32_t *px = SDL_malloc((size_t)w * 4);
	if (px) {
		uint32_t off = (uint32_t)((1.0f - 1.5f * ms) * 255.0f + 0.5f);
		for (int x = 0; x < w; x++) {
			int m = x % 3;
			px[x] = (m == 0 ? 255u : off) << 16 | (m == 1 ? 255u : off) << 8 | (m == 2 ? 255u : off);
		}
		SDL_UpdateTexture(grille, NULL, px, w * 4);
		SDL_free(px);
	}
	SDL_SetTextureBlendMode(grille, SDL_BLENDMODE_MOD);
	SDL_SetTextureScaleMode(grille, SDL_SCALEMODE_NEAREST);
}

static SDL_Texture *make_target(int w, int h, SDL_ScaleMode scale)
{
	SDL_Texture *t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
	if (t)
		SDL_SetTextureScaleMode(t, scale);
	return t;
}

int present_init(SDL_Window *win)
{
	if (present_headless()) {
		/* no renderer (present_common.h) */
		window = win;
		soft = 1;
		SDL_snprintf(name, sizeof name, "headless (software, no GPU)");
		return 1;
	}
	window = win;
	present_common_init(win);
	renderer = SDL_CreateRenderer(win, NULL);
	if (!renderer)
		return 0;
	SDL_SetRenderVSync(renderer, 1); /* best effort */
	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
				    SCREEN_W, SCREEN_H);
	if (!texture)
		return 0;
	SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
	vignette = make_vignette();
	glow_half = make_target(SCREEN_W / 2, SCREEN_H / 2, SDL_SCALEMODE_LINEAR);
	glow_quarter = make_target(SCREEN_W / 4, SCREEN_H / 4, SDL_SCALEMODE_LINEAR);
	if (glow_quarter) {
		SDL_SetTextureBlendMode(glow_quarter, SDL_BLENDMODE_ADD);
		SDL_SetTextureColorMod(glow_quarter, 24, 24, 24);
	}
	return 1;
}

/* Draw the current screen into the current target (ow x oh). */
static void draw(int ow, int oh)
{
	SDL_Texture *target = SDL_GetRenderTarget(renderer);
	int x, y, w, h;
	present_fit(ow, oh, &x, &y, &w, &h);
	SDL_FRect dst = { (float)x, (float)y, (float)w, (float)h };

	int mode = disp_mode, crt = mode <= DISPLAY_CRT_480;
	int bw = SCREEN_W, bh = SCREEN_H; /* the CRT raster: the game's, or CRT HD's virtual one */
	if (mode == DISPLAY_CRT_HD) {
		bh = disp_lines;
		bw = (disp_lines * 4 + 1) / 3;
	}
	int k = (h + bh - 1) / bh;
	if (k > MAX_K)
		k = MAX_K;
	/* the screen at an integer factor m, nearest */
	int m = (h + SCREEN_H - 1) / SCREEN_H;
	if (mode == DISPLAY_CRT_HD)
		m = (bh + SCREEN_H - 1) / SCREEN_H;
	if (m > 6)
		m = 6;
	if (mode != DISPLAY_CRT_480 && m != ipix_m) {
		if (ipix)
			SDL_DestroyTexture(ipix);
		ipix = make_target(SCREEN_W * m, SCREEN_H * m, SDL_SCALEMODE_LINEAR);
		ipix_m = ipix ? m : 0;
	}
	if (crt && (k != big_k || bw != big_w || bh != big_h)) {
		if (big)
			SDL_DestroyTexture(big);
		big = make_target(bw * k, bh * k, SDL_SCALEMODE_LINEAR);
		big_k = big ? k : 0;
		big_w = bw;
		big_h = bh;
	}
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
	if (mode != DISPLAY_CRT_480 && ipix) {
		SDL_SetRenderTarget(renderer, ipix);
		SDL_RenderTexture(renderer, texture, NULL, NULL);
		SDL_SetRenderTarget(renderer, target);
	}
	SDL_Texture *src = mode == DISPLAY_CRT_480 || !ipix ? texture : ipix;
	if (!crt || !big) {
		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
		SDL_RenderClear(renderer);
		SDL_RenderTexture(renderer, src, NULL, &dst);
		return;
	}
	if (k > 1 && (!scan_tex[k] || scan_lines[k] != bh)) {
		if (scan_tex[k])
			SDL_DestroyTexture(scan_tex[k]);
		scan_tex[k] = make_scanlines(k, bh);
		scan_lines[k] = bh;
	}
	/* 1: k-times picture with scanlines, and the glow source */
	SDL_SetRenderTarget(renderer, big);
	SDL_RenderTexture(renderer, src, NULL, NULL);
	if (scan_tex[k] && k > 1)
		SDL_RenderTexture(renderer, scan_tex[k], NULL, NULL);
	if (glow_half && glow_quarter) {
		SDL_SetRenderTarget(renderer, glow_half);
		SDL_RenderTexture(renderer, big, NULL, NULL);
		SDL_SetRenderTarget(renderer, glow_quarter);
		SDL_RenderTexture(renderer, glow_half, NULL, NULL);
	}
	SDL_SetRenderTarget(renderer, target);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	/* 2: picture, glow */
	SDL_RenderTexture(renderer, big, NULL, &dst);
	if (glow_quarter)
		SDL_RenderTexture(renderer, glow_quarter, NULL, &dst);
	/* 3: grille (faint at 2x, full from 3.5x, none at 1x), damper wires,
	 * vignette, then the glass's dark grey added under everything */
	float scale = (float)h / bh;
	float ms = 0.22f * SDL_clamp((scale - 1.25f) / 2.25f, 0.0f, 1.0f);
	if (ms > 0.01f) {
		make_grille(ow, ms);
		if (grille) {
			SDL_FRect src = { (float)x, 0, (float)w, 1 };
			SDL_RenderTexture(renderer, grille, &src, &dst);
		}
	}
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_MOD);
	float cover = SDL_min(1.0f, 0.3f * scale);
	Uint8 wire = (Uint8)((1.0f - 0.22f * cover) * 255.0f + 0.5f);
	SDL_SetRenderDrawColor(renderer, wire, wire, wire, 255);
	for (int i = 1; i <= 2; i++) {
		SDL_FRect r = { (float)x, (float)(y + h * i / 3), (float)w, 1 };
		SDL_RenderFillRect(renderer, &r);
	}
	if (vignette)
		SDL_RenderTexture(renderer, vignette, NULL, &dst);
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);
	SDL_SetRenderDrawColor(renderer, 7, 8, 9, 255);
	SDL_RenderFillRect(renderer, &dst);
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

void present_frame(const uint8_t *pixels, const uint32_t pal[256])
{
	if (soft) {
		soft_frame(pixels, pal);
		return;
	}
	for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
		rgba[i] = pal[pixels[i]];
	present_frame_rgb(rgba);
}

void present_frame_rgb(const uint32_t *xrgb)
{
	if (soft) {
		soft_frame_rgb(xrgb);
		return;
	}
	SDL_UpdateTexture(texture, NULL, xrgb, SCREEN_W * 4);
	present_note_in(present_luma_rgb(xrgb));
	int ow, oh;
	if (!SDL_GetCurrentRenderOutputSize(renderer, &ow, &oh) || ow <= 0 || oh <= 0) {
		pstats.skip_size++;
		return;
	}
	pstats.drawable_w = ow;
	pstats.drawable_h = oh;
	draw(ow, oh);
	Uint64 t = SDL_GetPerformanceCounter();
	SDL_RenderPresent(renderer);
	pstats.last_wait_ms = (double)(SDL_GetPerformanceCounter() - t) * 1000.0 /
			      (double)SDL_GetPerformanceFrequency();
	if (pstats.last_wait_ms > pstats.worst_wait_ms)
		pstats.worst_wait_ms = pstats.last_wait_ms;
	present_note_shown();
	present_common_tick();
}

void present_stats(struct present_stats *out)
{
	*out = pstats;
}

int present_capture(const char *bmp_path, int w, int h)
{
	if (soft)
		return soft_capture(bmp_path, w, h);
	if (w <= 0 || h <= 0)
		if (!SDL_GetCurrentRenderOutputSize(renderer, &w, &h))
			return 0;
	SDL_Texture *t = make_target(w, h, SDL_SCALEMODE_NEAREST);
	SDL_Surface *s = NULL;
	int ok = 0;
	if (t && SDL_SetRenderTarget(renderer, t)) {
		draw(w, h);
		SDL_Surface *raw = SDL_RenderReadPixels(renderer, NULL);
		if (raw) {
			s = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888);
			SDL_DestroySurface(raw);
		}
		ok = s && SDL_SaveBMP(s, bmp_path);
		SDL_SetRenderTarget(renderer, NULL);
	}
	SDL_DestroySurface(s);
	if (t)
		SDL_DestroyTexture(t);
	return ok;
}

const char *present_name(void)
{
	const char *rn = renderer ? SDL_GetRendererName(renderer) : NULL;
	if (rn)
		SDL_snprintf(name, sizeof name, "SDL_Renderer (%s)", rn);
	return name;
}

int present_window_to_screen(int wx, int wy, int *sx, int *sy)
{
	return present_map_point(window, wx, wy, sx, sy);
}

void present_screen_to_window(int sx, int sy, int *wx, int *wy)
{
	present_unmap_point(window, sx, sy, wx, wy);
}

int present_hotkey(const SDL_KeyboardEvent *key)
{
	return present_common_hotkey(window, key);
}

void present_set_fullscreen(int on)
{
	present_common_set_fullscreen(window, on);
}

int present_get_fullscreen(void)
{
	return present_common_get_fullscreen(window);
}

void present_shutdown(void)
{
	SDL_Texture **all[] = { &texture, &big, &ipix, &glow_half, &glow_quarter, &vignette, &grille };
	for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
		if (*all[i])
			SDL_DestroyTexture(*all[i]);
		*all[i] = NULL;
	}
	for (int k = 0; k <= MAX_K; k++) {
		if (scan_tex[k])
			SDL_DestroyTexture(scan_tex[k]);
		scan_tex[k] = NULL;
	}
	big_k = ipix_m = 0;
	grille_w = 0;
	if (renderer)
		SDL_DestroyRenderer(renderer);
	renderer = NULL;
}
