/* Cinepak decoder (public format, as documented by the Multimedia Wiki and
 * implemented by FFmpeg's cinepak.c):
 *
 *   frame:  u8 flags, u24 size, u16 width, u16 height, u16 strips
 *   strip:  u8 id (0x10 key, 0x11 inter), u24 size, u16 y0, u16 x0,
 *           u16 y1 (strip height), u16 x1, then chunks until size
 *   chunk:  u8 id, u24 size (with this 4-byte header)
 *     0x20/0x24 V4 codebook, 0x22/0x26 V1 codebook (all 256 entries);
 *     +1: selective update, a 32-bit flag word before every 32 entries;
 *     +4: grey entries (4 bytes, no u/v), else 6 bytes: y0..y3, u, v
 *     0x30 vectors with a V1/V4 bit per 4x4 block, 0x31 the same with a
 *     skip bit before it (inter frames), 0x32 all V1
 *
 * A V1 block is one codebook entry with each luma value covering 2x2
 * pixels; a V4 block is four entries, one per 2x2 quadrant. Codebooks are
 * kept per strip across frames; without flag bit 0 a strip starts from the
 * previous strip's codebooks. Colour: r = y + 2v, g = y - u/2 - v,
 * b = y + 2u. */
#include "cinepak.h"

#include <stdlib.h>
#include <string.h>

static unsigned be16(const uint8_t *p)
{
	return (unsigned)p[0] << 8 | p[1];
}

static unsigned be24(const uint8_t *p)
{
	return (unsigned)p[0] << 16 | (unsigned)p[1] << 8 | p[2];
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int cvid_init(struct cvid *c, int w, int h)
{
	memset(c, 0, sizeof *c);
	c->w = w;
	c->h = h;
	c->pixels = calloc((size_t)w * (size_t)h, 4);
	return c->pixels != NULL;
}

void cvid_free(struct cvid *c)
{
	free(c->pixels);
	c->pixels = NULL;
}

static void read_codebook(struct cvid_cb *cb, unsigned id, const uint8_t *p, const uint8_t *end)
{
	int n = id & 0x04 ? 4 : 6;
	uint32_t flag = 0, mask = 0;
	for (int i = 0; i < 256; i++) {
		if (id & 0x01) {
			if (!(mask >>= 1)) {
				if (p + 4 > end)
					return;
				flag = be32(p);
				p += 4;
				mask = 0x80000000u;
			}
			if (!(flag & mask))
				continue;
		}
		if (p + n > end)
			return;
		memcpy(cb[i].y, p, 4);
		cb[i].u = n == 6 ? (int8_t)p[4] : 0;
		cb[i].v = n == 6 ? (int8_t)p[5] : 0;
		p += n;
	}
}

static uint32_t rgb(int y, int u, int v)
{
	int r = y + 2 * v, g = y - u / 2 - v, b = y + 2 * u;
	r = r < 0 ? 0 : r > 255 ? 255 : r;
	g = g < 0 ? 0 : g > 255 ? 255 : g;
	b = b < 0 ? 0 : b > 255 ? 255 : b;
	return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

/* 2x2 pixels at (x, y) */
static void put2(struct cvid *c, int x, int y, uint32_t a, uint32_t b, uint32_t d, uint32_t e)
{
	for (int j = 0; j < 2; j++) {
		if (y + j >= c->h)
			break;
		uint32_t *row = c->pixels + (size_t)(y + j) * (size_t)c->w;
		if (x < c->w)
			row[x] = j ? d : a;
		if (x + 1 < c->w)
			row[x + 1] = j ? e : b;
	}
}

static void v1_block(struct cvid *c, int x, int y, const struct cvid_cb *cb)
{
	for (int q = 0; q < 4; q++) {
		uint32_t p = rgb(cb->y[q], cb->u, cb->v);
		put2(c, x + (q & 1) * 2, y + (q >> 1) * 2, p, p, p, p);
	}
}

static void v4_block(struct cvid *c, int x, int y, const struct cvid_cb *cb[4])
{
	for (int q = 0; q < 4; q++) {
		const struct cvid_cb *e = cb[q];
		put2(c, x + (q & 1) * 2, y + (q >> 1) * 2, rgb(e->y[0], e->u, e->v), rgb(e->y[1], e->u, e->v),
		     rgb(e->y[2], e->u, e->v), rgb(e->y[3], e->u, e->v));
	}
}

static int read_vectors(struct cvid *c, int s, unsigned id, int y0, int y1, int x0, int x1,
			const uint8_t *p, const uint8_t *end)
{
	uint32_t flag = 0, mask = 0;
	for (int y = y0; y < y1; y += 4)
		for (int x = x0; x < x1; x += 4) {
			if (id & 0x01) { /* skip bit */
				if (!(mask >>= 1)) {
					if (p + 4 > end)
						return 0;
					flag = be32(p);
					p += 4;
					mask = 0x80000000u;
				}
				if (!(flag & mask))
					continue;
			}
			int v4 = 0;
			if (!(id & 0x02)) { /* V1/V4 bit */
				if (!(mask >>= 1)) {
					if (p + 4 > end)
						return 0;
					flag = be32(p);
					p += 4;
					mask = 0x80000000u;
				}
				v4 = (flag & mask) != 0;
			}
			if (v4) {
				if (p + 4 > end)
					return 0;
				const struct cvid_cb *cb[4] = { &c->v4[s][p[0]], &c->v4[s][p[1]], &c->v4[s][p[2]],
								&c->v4[s][p[3]] };
				p += 4;
				v4_block(c, x, y, cb);
			} else {
				if (p + 1 > end)
					return 0;
				v1_block(c, x, y, &c->v1[s][*p++]);
			}
		}
	return 1;
}

int cvid_decode(struct cvid *c, const uint8_t *data, size_t size)
{
	if (size < 10)
		return 0;
	const uint8_t *end = data + size;
	unsigned flags = data[0];
	size_t fsize = be24(data + 1);
	if (fsize >= 10 && fsize < size)
		end = data + fsize;
	int strips = (int)be16(data + 8);
	if (strips > CVID_MAX_STRIPS)
		strips = CVID_MAX_STRIPS;
	const uint8_t *p = data + 10;
	int y = 0;
	for (int s = 0; s < strips; s++) {
		if (p + 12 > end)
			return 0;
		unsigned ssize = be24(p + 1);
		int height = (int)be16(p + 8);
		if (ssize < 12 || p + ssize > end)
			return 0;
		if (s > 0 && !(flags & 0x01)) {
			memcpy(c->v1[s], c->v1[s - 1], sizeof c->v1[s]);
			memcpy(c->v4[s], c->v4[s - 1], sizeof c->v4[s]);
		}
		const uint8_t *q = p + 12, *send = p + ssize;
		int y1 = y + height;
		while (q + 4 <= send) {
			unsigned id = q[0], csize = be24(q + 1);
			if (csize < 4 || q + csize > send)
				return 0;
			const uint8_t *body = q + 4, *cend = q + csize;
			switch (id) {
			case 0x20: case 0x21: case 0x24: case 0x25:
				read_codebook(c->v4[s], id, body, cend);
				break;
			case 0x22: case 0x23: case 0x26: case 0x27:
				read_codebook(c->v1[s], id, body, cend);
				break;
			case 0x30: case 0x31: case 0x32:
				read_vectors(c, s, id, y, y1, 0, c->w, body, cend);
				break;
			}
			q = cend;
		}
		y = y1;
		p = send;
	}
	return 1;
}
