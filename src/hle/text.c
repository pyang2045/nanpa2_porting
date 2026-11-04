/* GDI text for the lifted code. Native 0x401900 (char, x, y, size, fg, bg)
 * draws one Big5 or ASCII character with TextOutA at (0, 0) of the scratch
 * surface 0x4ab058 (font 0x4ac070[size & 3]: MingLiU at em heights 15, 24,
 * 32, weights 700, 700, 400; text colour 0xf0f0f0 on 0, opaque), then
 * copies an N x N cell (N = 8 * size + 16: 16, 24, 32) to the primary at
 * (x, y): fg where the scratch is non-zero, bg elsewhere, and clears the
 * scratch. There is no shadow pass. Glyphs come from a TrueType Ming font
 * via stb_truetype, unantialiased like GDI on an 8-bit DC; bold is GDI's
 * 1-pixel emboldening. Big5 is decoded as Windows code page 950. */
#include "hle.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#ifdef _WIN32
/* kernel32's MultiByteToWideChar (UINT, DWORD, LPCCH, int, LPWSTR, int),
 * declared here rather than with <windows.h> and its macros (min, max,
 * near, far, ERROR, ...) next to the emulator headers */
__declspec(dllimport) int __stdcall MultiByteToWideChar(unsigned int cp, unsigned long flags, const char *s,
							 int n, wchar_t *w, int wn);
#else
#include <iconv.h>
#endif

#define MAX_FONTS 8
#define INK 110 /* coverage threshold: GDI draws 8-bit DCs without antialiasing */
#define MAX_DC 16

static stbtt_fontinfo font;
static unsigned char *font_data;
static int have_font;

static struct {
	int height; /* em height in pixels (CreateFontA with negative height) */
	int bold;
	int base;   /* baseline row in the cell, 0: not computed yet */
} fonts[MAX_FONTS];
static int nfonts;

static struct {
	int font;
	uint32_t fg, bg;
	int opaque;
} dcs[MAX_DC];

static const char *const font_candidates[] = {
	"/System/Library/Fonts/Supplemental/Songti.ttc",
	"/Library/Fonts/Songti.ttc",
	"/usr/share/fonts/truetype/arphic/uming.ttc",
	"/usr/share/fonts/arphic-uming/uming.ttc",
	"/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
	"/usr/share/fonts/noto-cjk/NotoSerifCJK-Regular.ttc",
	"C:\\Windows\\Fonts\\mingliu.ttc",
	NULL,
};

static int load_font(const char *path)
{
	size_t n;
	void *d = SDL_LoadFile(path, &n);
	if (!d)
		return 0;
	/* pick the first face of a collection that covers Traditional-only
	 * characters (時 張 麼 聲 噹); name matching misses Songti TC */
	static const int probe[] = { 0x6642, 0x5f35, 0x9ebc, 0x8072, 0x5679 };
	int off = -1;
	int nf = stbtt_GetNumberOfFonts(d);
	for (int i = 0; i < (nf > 0 ? nf : 1) && off < 0; i++) {
		int o = stbtt_GetFontOffsetForIndex(d, i);
		stbtt_fontinfo fi;
		if (o < 0 || !stbtt_InitFont(&fi, d, o))
			continue;
		int ok = 1;
		for (size_t k = 0; k < sizeof probe / sizeof probe[0]; k++)
			ok &= stbtt_FindGlyphIndex(&fi, probe[k]) != 0;
		if (ok)
			off = o;
	}
	if (off < 0 || !stbtt_InitFont(&font, d, off)) {
		SDL_free(d);
		return 0;
	}
	font_data = d;
	have_font = 1;
	SDL_Log("Font: %s", path);
	return 1;
}

int text_init(const char *font_path)
{
	if (font_path && load_font(font_path))
		return 1;
	for (int i = 0; font_candidates[i]; i++)
		if (load_font(font_candidates[i]))
			return 1;
	SDL_Log("no CJK font found; text will be blank (use --font)");
	return 0;
}

uint32_t text_create_font(int height, int weight)
{
	if (nfonts >= MAX_FONTS)
		return 0;
	fonts[nfonts].height = height < 0 ? -height : height;
	fonts[nfonts].bold = weight >= 600;
	return 0x6000u + (uint32_t)nfonts++;
}

static int dc_index(uint32_t hdc)
{
	uint32_t i = hdc - 0x5000u;
	return i < MAX_DC ? (int)i : -1;
}

void text_select(uint32_t hdc, uint32_t f)
{
	int i = dc_index(hdc);
	if (i >= 0 && f >= 0x6000u && f < 0x6000u + (uint32_t)nfonts)
		dcs[i].font = (int)(f - 0x6000u);
}

void text_set_color(uint32_t hdc, uint32_t fg, uint32_t bg, int mode)
{
	int i = dc_index(hdc);
	if (i < 0)
		return;
	if (fg != 0xffffffffu)
		dcs[i].fg = fg;
	if (bg != 0xffffffffu)
		dcs[i].bg = bg;
	if (mode >= 0)
		dcs[i].opaque = mode == 2;
}

static uint32_t big5_to_ucs(const unsigned char *s, int len)
{
	if (len == 1)
		return s[0];
#ifdef _WIN32
	wchar_t w[2];
	if (MultiByteToWideChar(950, 0, (const char *)s, 2, w, 2) == 1)
		return w[0];
	return 0x3000;
#else
	static iconv_t cd = (iconv_t)-1;
	if (cd == (iconv_t)-1) {
		cd = iconv_open("UTF-32LE", "CP950");
		if (cd == (iconv_t)-1)
			cd = iconv_open("UTF-32LE", "BIG5");
		if (cd == (iconv_t)-1)
			return 0x3000;
	}
	char in[2] = { (char)s[0], (char)s[1] };
	unsigned char out[8];
	char *ip = in, *op = (char *)out;
	size_t il = 2, ol = sizeof out;
	iconv(cd, NULL, NULL, NULL, NULL);
	if (iconv(cd, &ip, &il, &op, &ol) == (size_t)-1 || ol != 4)
		return 0x3000;
	return out[0] | out[1] << 8 | out[2] << 16 | (uint32_t)out[3] << 24;
#endif
}

/* Thresholded ink rows of a glyph relative to the baseline. */
static void ink_rows(float scale, int ch, int *top, int *bot)
{
	int w, h, xo, yo;
	unsigned char *bm = stbtt_GetCodepointBitmap(&font, scale, scale, ch, &w, &h, &xo, &yo);
	if (!bm)
		return;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			if (bm[y * w + x] >= INK) {
				if (yo + y < *top) *top = yo + y;
				if (yo + y > *bot) *bot = yo + y;
			}
	stbtt_FreeBitmap(bm, NULL);
}

/* Baseline for an em-high font. GDI's TextOutA puts the top of the cell at
 * y, and native 0x401900 copies an N x N cell (N = 16, 24, 32 for the
 * heights 15, 24, 32 of 0x402620) to the screen: the baseline goes at the
 * font's ideographic ascender (OS/2 typo ascender), raised if ideographs
 * would reach past the cell's last row (Songti's ideographs fill the whole
 * em, so at 32 px they would lose their bottom row). */
static int baseline(int em)
{
	float scale = stbtt_ScaleForMappingEmToPixels(&font, (float)em);
	int asc, desc, gap;
	if (!stbtt_GetFontVMetricsOS2(&font, &asc, &desc, &gap))
		stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);
	int base = (int)(asc * scale + 0.5f);
	int cell = (em + 7) & ~7;
	static const int probe[] = { 0x570b, 0x9ad4, 0x8072, 0x9f8d, 0x53e3, 0x76ee, 0x9b31 };
	int top = 0, bot = 0;
	for (size_t i = 0; i < sizeof probe / sizeof probe[0]; i++)
		ink_rows(scale, probe[i], &top, &bot);
	if (base + bot > cell - 1)
		base = cell - 1 - bot;
	if (base + top < 0)
		base = -top;
	return base;
}

void text_out(uint32_t hdc, int x, int y, uint32_t str, int len)
{
	int di = dc_index(hdc);
	uint32_t pixels;
	int pitch, sw, sh;
	if (di < 0 || len <= 0 || !ddraw_surface_info(hdc, &pixels, &pitch, &sw, &sh))
		return;
	unsigned char s[2] = { (unsigned char)rd8(str), len > 1 ? (unsigned char)rd8(str + 1) : 0 };
	uint32_t ch = big5_to_ucs(s, len > 1 ? 2 : 1);
	if (getenv("NANPA2_LOG") && (ch == 0x3000 || (have_font && !stbtt_FindGlyphIndex(&font, (int)ch))))
		fprintf(stderr, "text: %02x %02x len %d -> U+%04X (missing)\n", s[0], s[1], len, ch);
	int em = fonts[dcs[di].font].height, bold = fonts[dcs[di].font].bold;
	if (have_font && !fonts[dcs[di].font].base)
		fonts[dcs[di].font].base = baseline(em);
	int base = fonts[dcs[di].font].base;
	/* GDI uses palette indices on an 8-bit DC; the native caller only tests
	 * for zero, so map any non-black colour to 0xff */
	uint32_t fg = dcs[di].fg ? 0xff : 0, bg = dcs[di].bg ? 0xff : 0;
	int cell_w = len > 1 ? em : (em + 1) / 2;

	if (dcs[di].opaque)
		for (int yy = 0; yy < em; yy++)
			for (int xx = 0; xx < cell_w + bold; xx++)
				if (y + yy >= 0 && y + yy < sh && x + xx >= 0 && x + xx < sw)
					wr8(pixels + (uint32_t)((y + yy) * pitch + x + xx), bg);
	if (!have_font || ch == ' ' || ch == 0x3000)
		return;

	float scale = stbtt_ScaleForMappingEmToPixels(&font, (float)em);
	int adv, lsb;
	stbtt_GetCodepointHMetrics(&font, (int)ch, &adv, &lsb);
	/* MingLiU's ASCII is fixed half-width; Songti's Latin is proportional
	 * and wider, so narrow it to the half-width cell (the next character's
	 * cell would cover the overhang: 0x401900 copies whole cells) */
	float sx = scale;
	if (len == 1 && adv * scale > cell_w)
		sx = cell_w / (float)adv;
	int gw, gh, xo, yo;
	unsigned char *bm = stbtt_GetCodepointBitmap(&font, sx, scale, (int)ch, &gw, &gh, &xo, &yo);
	if (!bm)
		return;
	int gx = x + (cell_w - (int)(adv * sx + 0.5f)) / 2 + xo;
	int gy = y + base + yo;
	for (int yy = 0; yy < gh; yy++)
		for (int xx = 0; xx < gw; xx++) {
			if (bm[yy * gw + xx] < INK)
				continue;
			for (int b = 0; b <= bold; b++) {
				int px = gx + xx + b, py = gy + yy;
				if (px >= 0 && px < sw && py >= 0 && py < sh)
					wr8(pixels + (uint32_t)(py * pitch + px), fg);
			}
		}
	stbtt_FreeBitmap(bm, NULL);
}
