/* GDI text for the lifted code (native 0x401900 draws one Big5 or ASCII
 * character at a time with TextOutA into a scratch surface, then copies the
 * non-zero pixels). Glyphs come from a TrueType Ming font via stb_truetype;
 * Big5 is decoded as Windows code page 950. */
#include "hle.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <iconv.h>
#endif

#define MAX_FONTS 8
#define MAX_DC 16

static stbtt_fontinfo font;
static unsigned char *font_data;
static int have_font;

static struct {
	int height; /* em height in pixels (CreateFontA with negative height) */
	int bold;
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
	int asc, desc, gap;
	stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);
	int base = (int)(asc * scale + 0.5f);
	/* fit the font's ascent+descent into the em box, as MingLiU does */
	int total = (int)((asc - desc) * scale + 0.5f);
	if (total > em)
		base -= (total - em + 1) / 2;
	int gw, gh, xo, yo;
	unsigned char *bm = stbtt_GetCodepointBitmap(&font, scale, scale, (int)ch, &gw, &gh, &xo, &yo);
	if (!bm)
		return;
	int adv, lsb;
	stbtt_GetCodepointHMetrics(&font, (int)ch, &adv, &lsb);
	int gx = x + (cell_w - (int)(adv * scale + 0.5f)) / 2 + xo;
	int gy = y + base + yo;
	for (int yy = 0; yy < gh; yy++)
		for (int xx = 0; xx < gw; xx++) {
			if (bm[yy * gw + xx] < 110)
				continue;
			for (int b = 0; b <= bold; b++) {
				int px = gx + xx + b, py = gy + yy;
				if (px >= 0 && px < sw && py >= 0 && py < sh)
					wr8(pixels + (uint32_t)(py * pitch + px), fg);
			}
		}
	stbtt_FreeBitmap(bm, NULL);
}
