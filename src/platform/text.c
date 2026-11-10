/* GDI text for the game. Native 0x401900 (char, x, y, size, fg, bg)
 * draws one Big5 or ASCII character with TextOutA at (0, 0) of the scratch
 * surface 0x4ab058 (font 0x4ac070[size & 3]: MingLiU at em heights 15, 24,
 * 32, weights 700, 700, 400; text colour 0xf0f0f0 on 0, opaque), then
 * copies an N x N cell (N = 8 * size + 16: 16, 24, 32) to the primary at
 * (x, y): fg where the scratch is non-zero, bg elsewhere, and clears the
 * scratch. There is no shadow pass. Glyphs come from a Ming font: its
 * embedded bitmap strike when it has one for the size (EBLC/EBDT, or Apple's
 * bloc/bdat; MingLiU and Apple LiSung have hand-made strikes), otherwise its
 * outline through stb_truetype, unantialiased like GDI on an 8-bit DC; bold
 * is GDI's 1-pixel emboldening. Big5 is decoded as Windows code page 950. */
#include "platform/text.h"
#include "platform/surface.h"
#include "platform/textlayer.h"

#include <SDL3/SDL.h>
#include "platform/docs.h"
#ifdef __APPLE__
#include "platform/font_apple.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
/* static stb functions this file does not use */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#ifdef _WIN32
/* kernel32's MultiByteToWideChar (UINT, DWORD, LPCCH, int, LPWSTR, int),
 * declared here rather than with <windows.h> and its macros (min, max,
 * near, far, ERROR, ...) next to the game's headers */
__declspec(dllimport) int __stdcall MultiByteToWideChar(unsigned int cp, unsigned long flags, const char *s,
							 int n, wchar_t *w, int wn);
#else
#include <dirent.h>
#include <iconv.h>
#endif

#define MAX_FONTS 8

/* Outline rasterization. A plain coverage threshold (INK) of an unhinted Ming
 * outline at 15 px gives strokes of 1 or 2 pixels at random and breaks or
 * merges the thin horizontals, which GDI's emboldening then turns into
 * blots. Instead a pixel is ink when its coverage is at least INK_HI, or at
 * least INK_LO and a ridge (a local maximum across the stroke, horizontally
 * or vertically): each stroke comes out one pixel wide, like a hand-made
 * bitmap. From RIDGE_MAX_EM up (the 32 px weight-400 font, drawn without
 * the bold) stems are about 2 pixels and the plain threshold keeps them.
 * NANPA2_TEXT=threshold restores the plain threshold everywhere and
 * NANPA2_TEXT=outline ignores bitmap strikes. */
#define INK 110
#define INK_LO 64
#define INK_HI 192
#define RIDGE_MAX_EM 28

/* faces[0] is the font; faces[1] a fallback for the characters it lacks
 * (Apple LiSung has no kana and no ～, U+FF5E from Big5 A1E3) */
#define MAX_FACES 2
_Static_assert(MAX_FONTS <= TL_MAX_FONTS && MAX_FACES <= TL_MAX_FACES, "textlayer.c keys fonts and faces in 3 bits");
static struct face {
	stbtt_fontinfo info;
	int off; /* of the face in data */
	unsigned char *data;
	size_t size;
	uint32_t loc, dat; /* embedded bitmap location and data tables (offsets into data), 0: none */
	char path[1024];
	int base[MAX_FONTS];   /* baseline row in the cell, 0: not computed yet */
	int strike[MAX_FONTS]; /* BitmapSize record (offset into data), 0: none, -1: not looked up */
} faces[MAX_FACES];
static int nfaces;
static int raster_threshold, no_strikes, font_missing;

int text_font_missing(void)
{
	return font_missing;
}

static struct {
	int height; /* em height in pixels (CreateFontA with negative height) */
	int bold;
} fonts[MAX_FONTS];
static int nfonts;

/* Apple LiSung Light (Dynalab's Ming, the foundry of MingLiU) is a macOS font
 * downloaded on demand to <asset dir>/<hash>.asset/AssetData; it has 1-bit
 * strikes at 9, 12, 16 and 24 pixels. */
static const char *const asset_dirs[] = {
	"/System/Library/AssetsV2/com_apple_MobileAsset_Font8",
	"/System/Library/AssetsV2/com_apple_MobileAsset_Font7",
	"/System/Library/AssetsV2/com_apple_MobileAsset_Font9",
	NULL,
};

/* '@': a file name looked up in asset_dirs */
static const char *const font_candidates[] = {
	"C:\\Windows\\Fonts\\mingliu.ttc",
	"/Library/Fonts/AppleLiSung-Light.ttf",
	"@AppleLiSung-Light.ttf",
	"/System/Library/Fonts/Supplemental/Songti.ttc",
	"/Library/Fonts/Songti.ttc",
	"/usr/share/fonts/truetype/arphic/uming.ttc",
	"/usr/share/fonts/arphic-uming/uming.ttc",
	"/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
	"/usr/share/fonts/noto-cjk/NotoSerifCJK-Regular.ttc",
	NULL,
};

/* A face's full name (name ID 4, Windows Unicode or Mac Roman) as ASCII. */
static void face_name(const stbtt_fontinfo *fi, char *out, int n)
{
	int len, k = 0;
	const char *s = stbtt_GetFontNameString(fi, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
						STBTT_MS_LANG_ENGLISH, 4);
	if (s) {
		for (int i = 1; i < len && k < n - 1; i += 2)
			out[k++] = s[i - 1] ? '?' : s[i];
	} else if ((s = stbtt_GetFontNameString(fi, &len, STBTT_PLATFORM_ID_MAC, 0, 0, 4))) {
		for (int i = 0; i < len && k < n - 1; i++)
			out[k++] = s[i];
	}
	out[k] = 0;
}

static int load_font_data(const char *path, void *d, size_t n);

/* Load the font file path as faces[nfaces]. */
static int load_font(const char *path)
{
	if (nfaces >= MAX_FACES)
		return 0;
	for (int i = 0; i < nfaces; i++)
		if (!strcmp(faces[i].path, path))
			return 0;
	size_t n;
	void *d = SDL_LoadFile(path, &n);
	if (!d)
		return 0;
	return load_font_data(path, d, n);
}

/* faces[nfaces] from the font file image d (SDL_malloc'd; taken over), named path in the log. */
static int load_font_data(const char *path, void *d, size_t n)
{
	/* Of the faces that cover Traditional-only characters (時 張 麼 聲 噹),
	 * take a Traditional Chinese one ("TC" or "HK" in the name: SC faces
	 * cover them too, with mainland glyph forms elsewhere) of regular weight
	 * (OS/2 usWeightClass nearest 400: MingLiU is a light Ming, and bold
	 * fonts get GDI's emboldening on top). The first covering face of
	 * Songti.ttc is Songti SC Bold. */
	static const int probe[] = { 0x6642, 0x5f35, 0x9ebc, 0x8072, 0x5679 };
	int off = -1, best = 1 << 30;
	char best_name[64] = "";
	int nf = stbtt_GetNumberOfFonts(d);
	for (int i = 0; i < (nf > 0 ? nf : 1); i++) {
		int o = stbtt_GetFontOffsetForIndex(d, i);
		stbtt_fontinfo fi;
		if (o < 0 || !stbtt_InitFont(&fi, d, o))
			continue;
		int ok = 1;
		for (size_t k = 0; k < sizeof probe / sizeof probe[0]; k++)
			ok &= stbtt_FindGlyphIndex(&fi, probe[k]) != 0;
		if (!ok)
			continue;
		char nm[64];
		face_name(&fi, nm, sizeof nm);
		stbtt_uint32 os2 = stbtt__find_table(d, (stbtt_uint32)o, "OS/2");
		int weight = os2 && os2 + 6 <= n ? ttUSHORT((unsigned char *)d + os2 + 4) : 400;
		int score = (strstr(nm, " TC") || strstr(nm, " HK") ? 0 : strstr(nm, " SC") ? 2000 : 1000) +
			    abs(weight - 400);
		if (score < best) {
			best = score;
			off = o;
			snprintf(best_name, sizeof best_name, "%s", nm);
		}
	}
	struct face *f = &faces[nfaces];
	if (off < 0 || !stbtt_InitFont(&f->info, d, off)) {
		SDL_free(d);
		return 0;
	}
	f->data = d;
	f->off = off;
	f->size = n;
	snprintf(f->path, sizeof f->path, "%s", path);
	f->loc = stbtt__find_table(d, (stbtt_uint32)off, "EBLC");
	f->dat = stbtt__find_table(d, (stbtt_uint32)off, "EBDT");
	if (!f->loc || !f->dat) {
		f->loc = stbtt__find_table(d, (stbtt_uint32)off, "bloc");
		f->dat = stbtt__find_table(d, (stbtt_uint32)off, "bdat");
	}
	if (!f->loc || !f->dat || f->loc + 8 > n)
		f->loc = f->dat = 0;
	for (int i = 0; i < MAX_FONTS; i++)
		f->strike[i] = -1;
	SDL_Log("Font%s: %s (%s%s)", nfaces ? " (fallback)" : "", path, best_name, f->loc ? ", bitmap strikes" : "");
	nfaces++;
	return 1;
}

#ifndef _WIN32
/* The font file name in one of asset_dirs' assets. */
static int load_asset_font(const char *name)
{
	for (int i = 0; asset_dirs[i]; i++) {
		DIR *dir = opendir(asset_dirs[i]);
		if (!dir)
			continue;
		struct dirent *e;
		int ok = 0;
		while (!ok && (e = readdir(dir))) {
			if (e->d_name[0] == '.')
				continue;
			char path[1024];
			snprintf(path, sizeof path, "%s/%s/AssetData/%s", asset_dirs[i], e->d_name, name);
			FILE *f = fopen(path, "rb");
			if (f) {
				fclose(f);
				ok = load_font(path);
			}
		}
		closedir(dir);
		if (ok)
			return 1;
	}
	return 0;
}
#endif

#ifdef __APPLE__
/* The system faces through CoreText, after every file path (macOS has the files; iOS only this). */
static int load_coretext(void)
{
	if (nfaces >= MAX_FACES)
		return 0;
	int tier = nfaces ? 1 : 0;
	for (int i = 0, n = font_apple_count(tier); i < n; i++) {
		char label[96];
		size_t size;
		snprintf(label, sizeof label, "CoreText:%s", font_apple_name(tier, i));
		int dup = 0;
		for (int k = 0; k < nfaces; k++)
			dup |= !strcmp(faces[k].path, label);
		void *d = dup ? NULL : font_apple_load(tier, i, &size, label, sizeof label);
		if (d && load_font_data(label, d, size))
			return 1;
	}
	return 0;
}
#endif

/* A user-supplied font in the app's Documents (iOS file sharing): .ttf/.ttc/.otf in Documents,
 * Documents/game or Documents/fonts, mingliu.ttc by name first (the original look, 15 px strike). */
#ifdef _WIN32
static int load_user_font(void)
{
	return 0;
}
#else
static int load_user_font(void)
{
	const char *docs = docs_dir();
	if (!docs)
		return 0;
	static const char *const sub[] = { "", "/game", "/fonts", NULL };
	char found[1024] = "";
	int rank = 99;
	for (int s = 0; sub[s]; s++) {
		char dirp[1024];
		snprintf(dirp, sizeof dirp, "%s%s", docs, sub[s]);
		DIR *dir = opendir(dirp);
		if (!dir)
			continue;
		struct dirent *e;
		while ((e = readdir(dir))) {
			size_t l = strlen(e->d_name);
			if (e->d_name[0] == '.' || l < 5)
				continue;
			const char *x = e->d_name + l - 4;
			if (SDL_strcasecmp(x, ".ttf") && SDL_strcasecmp(x, ".ttc") && SDL_strcasecmp(x, ".otf"))
				continue;
			int r = !SDL_strcasecmp(e->d_name, "mingliu.ttc") ? 0 :
				!SDL_strcasecmp(e->d_name, "mingliu.ttf") ? 1 : 2;
			char cand[1024];
			if (r < rank && snprintf(cand, sizeof cand, "%s/%s", dirp, e->d_name) < (int)sizeof cand) {
				memcpy(found, cand, sizeof found);
				rank = r;
			}
		}
		closedir(dir);
	}
	if (rank == 99 || !load_font(found))
		return 0;
	SDL_Log("Font: user-supplied %s", found);
	return 1;
}
#endif

/* The first loadable candidate not loaded yet. */
static int load_candidate(void)
{
	const char *mode = getenv("NANPA2_TEXT"); /* "coretext": skip the file paths (tests the iOS route) */
	for (int i = 0; font_candidates[i] && !(mode && strstr(mode, "coretext")); i++) {
		if (font_candidates[i][0] == '@') {
#ifndef _WIN32
			if (load_asset_font(font_candidates[i] + 1))
				return 1;
#endif
			continue;
		}
		if (load_font(font_candidates[i]))
			return 1;
	}
#ifdef __APPLE__
	return load_coretext();
#else
	return 0;
#endif
}

int text_init(const char *font_path)
{
	const char *mode = getenv("NANPA2_TEXT");
	raster_threshold = mode && strstr(mode, "threshold");
	no_strikes = mode && strstr(mode, "outline");
	if (!(font_path && load_font(font_path)) && !load_user_font() && !load_candidate()) {
		SDL_Log("WARNING: no CJK font found; text will be blank (use --font, or on iOS put a .ttf/.ttc in Documents)");
		font_missing = 1;
		return 0;
	}
	load_candidate(); /* the fallback */
	return 1;
}

uint32_t text_font_create(int height, int weight)
{
	if (nfonts >= MAX_FONTS)
		return 0;
	fonts[nfonts].height = height < 0 ? -height : height;
	fonts[nfonts].bold = weight >= 600;
	return 0x6000u + (uint32_t)nfonts++;
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

/* The face text_out is drawing from (set there; the helpers below read it). */
static struct face *F;

/* ---- embedded bitmap strikes (OpenType EBLC/EBDT, Apple bloc/bdat) ---- */

static int in_font(uint32_t o, uint32_t len)
{
	return o <= F->size && len <= F->size - o;
}

static uint32_t be32(uint32_t o)
{
	return in_font(o, 4) ? ttULONG(F->data + o) : 0;
}

static unsigned be16(uint32_t o)
{
	return in_font(o, 2) ? ttUSHORT(F->data + o) : 0;
}

/* The BitmapSize record of the 1-bit strike of ppem pixels, 0 if none. */
static int find_strike(int ppem)
{
	uint32_t n = be32(F->loc + 4);
	for (uint32_t i = 0; i < n && i < 256; i++) {
		uint32_t r = F->loc + 8 + 48 * i;
		if (!in_font(r, 48))
			break;
		if (F->data[r + 45] == ppem && F->data[r + 46] == 1)
			return (int)r;
	}
	return 0;
}

struct sbit {
	int w, h, bx, by, adv; /* bearings from the pen position on the baseline, y up */
	uint32_t bits;          /* image data offset */
	int byte_aligned;       /* rows padded to bytes (image formats 1, 6) */
};

static void sbit_metrics(uint32_t o, struct sbit *g)
{
	g->h = F->data[o];
	g->w = F->data[o + 1];
	g->bx = (signed char)F->data[o + 2];
	g->by = (signed char)F->data[o + 3];
	g->adv = F->data[o + 4];
}

/* Glyph gid of the strike at record r: index subtable formats 1-5, image
 * formats 1, 2, 5, 6, 7 (Apple LiSung: 1+6 and 2+5). */
static int sbit_glyph(uint32_t r, int gid, struct sbit *g)
{
	if (gid < (int)be16(r + 40) || gid > (int)be16(r + 42))
		return 0;
	uint32_t arr = F->loc + be32(r), nsub = be32(r + 8);
	for (uint32_t k = 0; k < nsub && in_font(arr + 8 * k, 8); k++) {
		uint32_t e = arr + 8 * k;
		int first = (int)be16(e), last = (int)be16(e + 2);
		if (gid < first || gid > last)
			continue;
		uint32_t st = arr + be32(e + 4);
		unsigned ifmt = be16(st), imfmt = be16(st + 2);
		uint32_t base = F->dat + be32(st + 4), off = 0;
		int i = gid - first, have_met = 0;
		switch (ifmt) {
		case 1: {
			uint32_t o0 = be32(st + 8 + 4 * i), o1 = be32(st + 12 + 4 * i);
			if (o1 <= o0)
				return 0;
			off = base + o0;
			break;
		}
		case 3: {
			uint32_t o0 = be16(st + 8 + 2 * i), o1 = be16(st + 10 + 2 * i);
			if (o1 <= o0)
				return 0;
			off = base + o0;
			break;
		}
		case 2:
			if (!in_font(st + 12, 8))
				return 0;
			off = base + be32(st + 8) * (uint32_t)i;
			sbit_metrics(st + 12, g);
			have_met = 1;
			break;
		case 4: {
			uint32_t ng = be32(st + 8), j;
			for (j = 0; j < ng && be16(st + 12 + 4 * j) != (unsigned)gid; j++)
				;
			if (j >= ng || be16(st + 18 + 4 * j) <= be16(st + 14 + 4 * j))
				return 0;
			off = base + be16(st + 14 + 4 * j);
			break;
		}
		case 5: {
			uint32_t ng = be32(st + 20), j;
			for (j = 0; j < ng && be16(st + 24 + 2 * j) != (unsigned)gid; j++)
				;
			if (j >= ng || !in_font(st + 12, 8))
				return 0;
			off = base + be32(st + 8) * j;
			sbit_metrics(st + 12, g);
			have_met = 1;
			break;
		}
		default:
			return 0;
		}
		if (imfmt == 1 || imfmt == 2) {
			if (!in_font(off, 5))
				return 0;
			sbit_metrics(off, g);
			off += 5;
		} else if (imfmt == 6 || imfmt == 7) {
			if (!in_font(off, 8))
				return 0;
			sbit_metrics(off, g);
			off += 8;
		} else if (imfmt != 5 || !have_met) {
			return 0;
		}
		g->bits = off;
		g->byte_aligned = imfmt == 1 || imfmt == 6;
		uint32_t bytes = g->byte_aligned ? (uint32_t)((g->w + 7) / 8 * g->h) : (uint32_t)((g->w * g->h + 7) / 8);
		return in_font(off, bytes);
	}
	return 0;
}

static int sbit_pixel(const struct sbit *g, int x, int y)
{
	uint32_t i = g->byte_aligned ? (uint32_t)(y * ((g->w + 7) / 8) * 8 + x) : (uint32_t)(y * g->w + x);
	return F->data[g->bits + i / 8] >> (7 - i % 8) & 1;
}

/* ---- outlines ---- */

/* Coverage (w x h) to ink in place: 1 ink, 0 paper (see INK_LO). */
static void ink_mask(unsigned char *bm, int w, int h, int em)
{
	unsigned char *c = malloc((size_t)w * (size_t)h);
	if (!c)
		return;
	memcpy(c, bm, (size_t)w * (size_t)h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int v = c[y * w + x];
			int ink;
			if (raster_threshold || em >= RIDGE_MAX_EM) {
				ink = v >= INK;
			} else {
				int l = x > 0 ? c[y * w + x - 1] : 0, r = x < w - 1 ? c[y * w + x + 1] : 0;
				int u = y > 0 ? c[(y - 1) * w + x] : 0, d = y < h - 1 ? c[(y + 1) * w + x] : 0;
				ink = v >= INK_HI || (v >= INK_LO && ((v >= l && v > r) || (v >= u && v > d)));
			}
			bm[y * w + x] = (unsigned char)ink;
		}
	free(c);
}

/* Ink rows of a glyph relative to the baseline. */
static void ink_rows(float scale, int em, int ch, int *top, int *bot)
{
	int w, h, xo, yo;
	unsigned char *bm = stbtt_GetCodepointBitmap(&F->info, scale, scale, ch, &w, &h, &xo, &yo);
	if (!bm)
		return;
	ink_mask(bm, w, h, em);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			if (bm[y * w + x]) {
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
	float scale = stbtt_ScaleForMappingEmToPixels(&F->info, (float)em);
	int asc, desc, gap;
	if (!stbtt_GetFontVMetricsOS2(&F->info, &asc, &desc, &gap))
		stbtt_GetFontVMetrics(&F->info, &asc, &desc, &gap);
	int base = (int)(asc * scale + 0.5f);
	int cell = (em + 7) & ~7;
	static const int probe[] = { 0x570b, 0x9ad4, 0x8072, 0x9f8d, 0x53e3, 0x76ee, 0x9b31 };
	int top = 0, bot = 0;
	for (size_t i = 0; i < sizeof probe / sizeof probe[0]; i++)
		ink_rows(scale, em, probe[i], &top, &bot);
	if (base + bot > cell - 1)
		base = cell - 1 - bot;
	if (base + top < 0)
		base = -top;
	return base;
}

/* The strike for an em-high font: one of em pixels (GDI's choice), else one
 * of the cell size (Apple LiSung has 16 for MingLiU's 15: its ideographs
 * span columns 1-14 and rows 0-15 of 16, so the emboldened glyph fits the
 * 16-pixel cell of 0x401900). */
static int strike_for(int em)
{
	if (!F->loc || no_strikes)
		return 0;
	int r = find_strike(em);
	return r ? r : find_strike((em + 7) & ~7);
}

/* What the last text_draw rendered, for the text layer (platform/textlayer.h). */
static struct tl_glyph cap;

static void text_draw_impl(uint32_t surface, int x, int y, uint32_t font, const uint8_t *str, int len,
			   uint32_t fg_colour, uint32_t bg_colour, int opaque)
{
	int pitch, sw, sh;
	uint8_t *pixels = surf_lock(surface, &pitch);
	if (len <= 0 || !surf_size(surface, &sw, &sh))
		return;
	unsigned char s[2] = { str[0], len > 1 ? str[1] : 0 };
	uint32_t ch = big5_to_ucs(s, len > 1 ? 2 : 1);
	/* the first face that has the character */
	int have_font = nfaces > 0;
	F = &faces[0];
	for (int i = 0; i < nfaces; i++)
		if (stbtt_FindGlyphIndex(&faces[i].info, (int)ch)) {
			F = &faces[i];
			break;
		}
	if (getenv("NANPA2_LOG") && (ch == 0x3000 || (have_font && !stbtt_FindGlyphIndex(&F->info, (int)ch))))
		fprintf(stderr, "text: %02x %02x len %d -> U+%04X (missing)\n", s[0], s[1], len, ch);
	int fi = font >= 0x6000u && font < 0x6000u + (uint32_t)nfonts ? (int)(font - 0x6000u) : 0;
	int em = fonts[fi].height, bold = fonts[fi].bold;
	if (have_font && !F->base[fi])
		F->base[fi] = baseline(em);
	if (have_font && F->strike[fi] < 0)
		F->strike[fi] = strike_for(em);
	int base = F->base[fi], strike = have_font ? F->strike[fi] : 0;
	/* GDI uses palette indices on an 8-bit DC; the native caller only tests
	 * for zero, so map any non-black colour to 0xff */
	uint32_t fg = fg_colour ? 0xff : 0, bg = bg_colour ? 0xff : 0;
	int cell_w = len > 1 ? em : (em + 1) / 2;
	cap.cp = ch;
	cap.face = (uint8_t)(F - faces);
	cap.fi = (uint8_t)fi;
	cap.len = (uint8_t)len;
	cap.em = (uint8_t)em;
	cap.bold = (uint8_t)bold;
	cap.cell_w = (uint8_t)cell_w;
	cap.base = (float)base;
	cap.cx = cell_w / 2.0f;

	if (opaque)
		for (int yy = 0; yy < em; yy++)
			for (int xx = 0; xx < cell_w + bold; xx++)
				if (y + yy >= 0 && y + yy < sh && x + xx >= 0 && x + xx < sw)
					pixels[(y + yy) * pitch + x + xx] = (uint8_t)bg;
	if (!have_font || ch == ' ' || ch == 0x3000)
		return;

	/* bitmap strike; proportional Latin wider than the half-width cell
	 * goes through the narrowed outline below */
	struct sbit sb;
	if (strike > 0 && sbit_glyph((uint32_t)strike, stbtt_FindGlyphIndex(&F->info, (int)ch), &sb) &&
	    (len > 1 || sb.adv <= cell_w)) {
		int sbase = (signed char)F->data[strike + 16]; /* hori.ascender */
		cap.em = F->data[strike + 45];
		cap.base = (float)sbase;
		cap.cx = (float)((cell_w - sb.adv) / 2) + sb.adv / 2.0f; /* int division as below */
		int gx = x + (cell_w - sb.adv) / 2 + sb.bx, gy = y + sbase - sb.by;
		for (int yy = 0; yy < sb.h; yy++)
			for (int xx = 0; xx < sb.w; xx++) {
				if (!sbit_pixel(&sb, xx, yy))
					continue;
				for (int b = 0; b <= bold; b++) {
					int px = gx + xx + b, py = gy + yy;
					if (px >= 0 && px < sw && py >= 0 && py < sh)
						pixels[py * pitch + px] = (uint8_t)fg;
				}
			}
		return;
	}

	float scale = stbtt_ScaleForMappingEmToPixels(&F->info, (float)em);
	int adv, lsb;
	stbtt_GetCodepointHMetrics(&F->info, (int)ch, &adv, &lsb);
	/* MingLiU's ASCII is fixed half-width; Songti's Latin is proportional
	 * and wider, so narrow it to the half-width cell (the next character's
	 * cell would cover the overhang: 0x401900 copies whole cells) */
	float sx = scale;
	if (len == 1 && adv * scale > cell_w)
		sx = cell_w / (float)adv;
	int gw, gh, xo, yo;
	unsigned char *bm = stbtt_GetCodepointBitmap(&F->info, sx, scale, (int)ch, &gw, &gh, &xo, &yo);
	if (!bm)
		return;
	ink_mask(bm, gw, gh, em);
	int gx = x + (cell_w - (int)(adv * sx + 0.5f)) / 2 + xo;
	int gy = y + base + yo;
	cap.cx = (float)((cell_w - (int)(adv * sx + 0.5f)) / 2) + (int)(adv * sx + 0.5f) / 2.0f;
	for (int yy = 0; yy < gh; yy++)
		for (int xx = 0; xx < gw; xx++) {
			if (!bm[yy * gw + xx])
				continue;
			for (int b = 0; b <= bold; b++) {
				int px = gx + xx + b, py = gy + yy;
				if (px >= 0 && px < sw && py >= 0 && py < sh)
					pixels[py * pitch + px] = (uint8_t)fg;
			}
		}
	stbtt_FreeBitmap(bm, NULL);
}

void text_draw(uint32_t surface, int x, int y, uint32_t font, const uint8_t *str, int len, uint32_t fg_colour,
	       uint32_t bg_colour, int opaque)
{
	memset(&cap, 0, sizeof cap);
	text_draw_impl(surface, x, y, font, str, len, fg_colour, bg_colour, opaque);
	/* the cell as the game is about to copy it: ink is a non-zero pixel */
	int pitch, sw, sh;
	if (!cap.cell_w || !surf_size(surface, &sw, &sh)) {
		tl_glyph_drawn(&cap); /* nothing drawn: n is 0 */
		return;
	}
	int n = (fonts[cap.fi].height + 7) & ~7;
	if (n > 32 || x < 0 || y < 0 || x + n > sw || y + n > sh) {
		memset(&cap, 0, sizeof cap);
		tl_glyph_drawn(&cap);
		return;
	}
	const uint8_t *px = surf_lock(surface, &pitch);
	cap.n = (uint8_t)n;
	for (int r = 0; r < n; r++)
		for (int c = 0; c < n; c++)
			if (px[(y + r) * pitch + x + c])
				cap.mask[r] |= 1u << c;
	tl_glyph_drawn(&cap);
}

/* The font face the text layer rasterises from (textraster.c): its file image and the offset of the face
 * in it. 0 if there is no such face. */
int text_face_font(int face, const unsigned char **data, int *offset)
{
	if (face < 0 || face >= nfaces)
		return 0;
	*data = faces[face].data;
	*offset = faces[face].off;
	return 1;
}

void text_cell_placed(uint32_t surface, int x, int y, int n, uint8_t fg, uint8_t bg)
{
	tl_cell_placed(surface, x, y, n, fg, bg);
}
