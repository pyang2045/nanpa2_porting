#include "gp4.h"
#include "fsys.h"

#include <stdlib.h>
#include <string.h>

static unsigned rd16(const unsigned char *p)
{
	return p[0] | (p[1] << 8);
}

int gp4_load(const char *name, struct gp4 *out)
{
	size_t n;
	unsigned char *b = fsys_load_data(name, &n);
	memset(out, 0, sizeof *out);
	if (!b || n < 520) {
		free(b);
		return 0;
	}
	out->x = rd16(b);
	out->y = rd16(b + 2);
	out->w = rd16(b + 4);
	out->h = rd16(b + 6);
	for (int i = 0; i < 256; i++)
		out->pal_raw[i] = (uint16_t)rd16(b + 8 + 2 * i);
	size_t px = (size_t)out->w * out->h;
	if (n < 520 + px) {
		free(b);
		return 0;
	}
	out->pixels = malloc(px ? px : 1);
	memcpy(out->pixels, b + 520, px);
	free(b);
	return 1;
}

void gp4_free(struct gp4 *g)
{
	free(g->pixels);
	g->pixels = NULL;
}

void gp4_palette_rgb(const uint16_t raw[256], uint32_t rgb[256])
{
	for (int i = 0; i < 256; i++) {
		unsigned c = raw[i], r, g, b;
		if (i < 16) {
			r = c & 0xf0;
			g = (c >> 4) & 0xf0;
			b = (c << 4) & 0xf0;
		} else {
			r = (c >> 7) & 0xf8;
			g = (c >> 2) & 0xf8;
			b = (c << 3) & 0xf8;
		}
		rgb[i] = (r << 16) | (g << 8) | b;
	}
}
