/* The text layer's glyphs: a face's outline, antialiased, at the display's resolution (stb_truetype).
 * Built -O2 whatever the build type (text.c, which makes the game's own ink masks, keeps the project's
 * flags; stb_truetype is compiled again here, static). */
#include "platform/text.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

/* the faces text.c loaded, set up here once */
int text_face_font(int face, const unsigned char **data, int *offset);

static const stbtt_fontinfo *face_info(int face)
{
	static stbtt_fontinfo infos[TL_MAX_FACES];
	static signed char state[TL_MAX_FACES]; /* 0 not tried, 1 ready, -1 none */
	if (face < 0 || face >= TL_MAX_FACES)
		return NULL;
	if (!state[face]) {
		const unsigned char *data;
		int off;
		state[face] = text_face_font(face, &data, &off) && stbtt_InitFont(&infos[face], data, off) ? 1 : -1;
	}
	return state[face] > 0 ? &infos[face] : NULL;
}


/* GDI's bold copies the glyph one game pixel to the right; the vector glyph is widened by this much of it
 * (NANPA2_TEXT_SMEAR): a full pixel fuses the strokes of dense ideographs at the display's resolution */
#define TEXT_SMEAR 0.75f

/* The coverage bitmap of glyph g at `scale` output pixels per game pixel, widened for the bold
 * the way GDI's emboldening widens it: copied one game pixel to the right, so every column of
 * the result is the maximum over that shift. *xo, *yo: its top left from the pen on the baseline
 * (y up is negative), in output pixels; *pen_x: the pen's distance from the cell's left, in game
 * pixels (the glyph's advance centred in the cell, as text_draw does it). malloc'd, NULL if the
 * face has no such glyph or draws nothing. */
static uint8_t *hires_raw(const struct tl_glyph *g, float scale, int *w, int *h, int *xo, int *yo, float *pen_x)
{
	const stbtt_fontinfo *fi = face_info(g->face);
	if (!fi)
		return NULL;
	float low = stbtt_ScaleForMappingEmToPixels(fi, (float)g->em);
	float sy = low * scale, sx = sy;
	int adv, lsb;
	stbtt_GetCodepointHMetrics(fi, (int)g->cp, &adv, &lsb);
	/* the narrowing of wide Latin letters (see text_draw_impl) */
	if (g->len == 1 && adv * low > g->cell_w)
		sx = sy * g->cell_w / (adv * low);
	int gw, gh, ox, oy;
	unsigned char *bm = stbtt_GetCodepointBitmap(fi, sx, sy, (int)g->cp, &gw, &gh, &ox, &oy);
	if (!bm)
		return NULL;
	/* the advance box centred where the bitmap's was (the strike's advance is a pixel wider than the
	 * font's, and the division by 2 truncates) */
	*pen_x = g->cx - adv * sx / scale / 2;
	*xo = ox;
	*yo = oy;
	if (!g->bold) {
		unsigned char *o = malloc((size_t)gw * (size_t)gh);
		if (o)
			memcpy(o, bm, (size_t)gw * (size_t)gh);
		stbtt_FreeBitmap(bm, NULL);
		*w = gw;
		*h = gh;
		return o;
	}
	static float smear = -1;
	if (smear < 0)
		smear = getenv("NANPA2_TEXT_SMEAR") ? (float)atof(getenv("NANPA2_TEXT_SMEAR")) : TEXT_SMEAR;
	float sm = scale * smear; /* output pixels of the emboldening */
	int m = (int)ceilf(sm) < 1 ? 1 : (int)ceilf(sm);
	int ow = gw + m;
	unsigned char *o = calloc((size_t)ow, (size_t)gh);
	if (!o) {
		stbtt_FreeBitmap(bm, NULL);
		return NULL;
	}
	for (int y = 0; y < gh; y++) {
		const unsigned char *row = bm + y * gw;
		for (int x = 0; x < ow; x++) {
			float best = 0;
			for (int j = 0; j <= m; j++) {
				float xf = x - j * sm / m;
				int i0 = (int)floorf(xf);
				float t = xf - (float)i0;
				float a = i0 >= 0 && i0 < gw ? row[i0] : 0, b = i0 + 1 >= 0 && i0 + 1 < gw ? row[i0 + 1] : 0;
				float v = a + (b - a) * t;
				if (v > best)
					best = v;
			}
			o[y * ow + x] = (unsigned char)(best + 0.5f);
		}
	}
	stbtt_FreeBitmap(bm, NULL);
	*w = ow;
	*h = gh;
	return o;
}

/* The strokes of the original's bitmap glyphs are a whole pixel wide where the outline's are
 * thinner (a light Ming), so the coverage is raised a little (an exponent below 1 on it) to
 * keep the weight of the text; NANPA2_TEXT_WEIGHT=1 leaves it as the outline has it. */
#define TEXT_WEIGHT 0.8f

uint8_t *text_hires_glyph(const struct tl_glyph *g, float scale, int *w, int *h, int *xo, int *yo, float *pen_x)
{
	static float weight;
	static uint8_t lut[256];
	if (weight == 0) {
		weight = getenv("NANPA2_TEXT_WEIGHT") ? (float)atof(getenv("NANPA2_TEXT_WEIGHT")) : TEXT_WEIGHT;
		if (!(weight > 0))
			weight = TEXT_WEIGHT;
		for (int i = 0; i < 256; i++)
			lut[i] = (uint8_t)(255.0f * powf((float)i / 255.0f, weight) + 0.5f);
	}
	uint8_t *bm = hires_raw(g, scale, w, h, xo, yo, pen_x);
	if (bm && weight != 1.0f)
		for (int i = 0; i < *w * *h; i++)
			bm[i] = lut[bm[i]];
	return bm;
}

void text_hires_free(uint8_t *bm)
{
	free(bm);
}

