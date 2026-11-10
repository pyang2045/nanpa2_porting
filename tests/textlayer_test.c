/* Unit test of the text layer's tracking and validation (src/platform/textlayer.c) with a stubbed
 * rasteriser and no game data: what is shown when the screen is as the game drew it, when part of it
 * was drawn over (the pointer), when the cell was cleared or drawn over, after a fill, after a BltFast
 * that moved it, and with another palette. Exit 0 = pass. */
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/surface.h"
#include "platform/text.h"
#include "platform/textlayer.h"

#define SCREEN_W 640
#define SCREEN_H 480

/* ---- stubs of the rest of the platform ---- */
uint64_t clock_host_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static int g_calls;  /* rasteriser calls */
static int g_empty;   /* the stub rasteriser finds no outline */
static int g_big = 1; /* glyph size factor, to make the atlas fill */
static int g_ptr, g_px, g_py; /* the pointer the validation is told of */

int surf_size(uint32_t s, int *w, int *h)
{
	(void)s;
	*w = 640;
	*h = 480;
	return 1;
}

uint8_t *text_hires_glyph(const struct tl_glyph *g, float scale, int *w, int *h, int *xo, int *yo, float *pen_x)
{
	(void)g;
	g_calls++;
	if (g_empty)
		return NULL;
	*w = *h = (int)(8 * scale) * g_big;
	*xo = 0;
	*yo = -*h;
	*pen_x = 1;
	uint8_t *p = malloc((size_t)*w * (size_t)*h);
	memset(p, 255, (size_t)*w * (size_t)*h);
	return p;
}

void text_hires_free(uint8_t *p)
{
	free(p);
}

static int failures;
#define CHECK(c, ...)                                                                                              \
	do {                                                                                                       \
		if (!(c)) {                                                                                        \
			failures++;                                                                                \
			fprintf(stderr, "FAIL line %d: ", __LINE__);                                                \
			fprintf(stderr, __VA_ARGS__);                                                               \
			fprintf(stderr, "\n");                                                                      \
		}                                                                                                  \
	} while (0)

static uint8_t frame[SCREEN_W * SCREEN_H];
static uint32_t pal[256];

/* a 16 x 16 Big5 glyph: ink on a diagonal band, so ink pixels are plentiful */
static struct tl_glyph make_glyph(uint32_t cp)
{
	struct tl_glyph g;
	memset(&g, 0, sizeof g);
	g.cp = cp;
	g.len = 2;
	g.em = 15;
	g.n = 16;
	g.cell_w = 15;
	g.base = 13;
	g.cx = 7.5f;
	for (int r = 0; r < 16; r++)
		g.mask[r] = 0x7u << (r % 12);
	return g;
}

/* what the game does: the glyph, the copy of its cell to the screen at (x, y), then the note */
static void draw(int x, int y, uint32_t cp, int fg, int bg)
{
	struct tl_glyph g = make_glyph(cp);
	tl_glyph_drawn(&g);
	for (int r = 0; r < 16; r++)
		for (int c = 0; c < 16; c++)
			frame[(y + r) * SCREEN_W + x + c] = (uint8_t)(g.mask[r] >> c & 1 ? fg : bg);
	tl_cell_placed(SURF_PRIMARY_HANDLE, x, y, 16, fg, bg);
}

static int shown(void)
{
	tl_validate(frame, pal, g_ptr, g_px, g_py);
	struct tl_stats st;
	const struct tl_quad *q;
	int n = tl_layout(2.0f, 2.0f, -1, &q);
	tl_get_stats(&st);
	(void)n;
	return st.shown;
}

int main(void)
{
	pal[7] = 0xf0f0f0;
	pal[12] = 0x90b0c0;
	tl_set_mode(TEXT_HIRES);
	int hold = shown();
	CHECK(hold == 0, "empty list shows %d", hold);

	draw(100, 100, 0x6559, 7, 12);
	draw(116, 100, 0x5ba4, 7, 12);
	CHECK(shown() == 2, "two cells as drawn: %d", shown());

	/* the quads: the cell rectangle at 2x, colours in linear light from the palette */
	{
		const struct tl_quad *q;
		int n = tl_layout(2.0f, 2.0f, -1, &q);
		CHECK(n == 2, "quads %d", n);
		CHECK(q[0].x0 == 200 && q[0].y0 == 200 && q[0].x1 == 232 && q[0].y1 == 232, "cell rect %g,%g-%g,%g", q[0].x0,
		      q[0].y0, q[0].x1, q[0].y1);
		CHECK(q[0].fg[0] > 0.85f && q[0].fg[0] < 0.9f, "fg linear %g", q[0].fg[0]);
		CHECK(q[0].gw > 0, "glyph rectangle");
		/* below the factor 1.5 there is no layer */
		CHECK(tl_layout(1.0f, 1.0f, -1, &q) == 0, "1x draws nothing");
	}

	/* the same indices with another palette: the colours follow, the cells stay */
	pal[7] = 0x101010;
	pal[12] = 0x000000;
	{
		const struct tl_quad *q;
		tl_validate(frame, pal, g_ptr, g_px, g_py);
		tl_layout(2.0f, 2.0f, -1, &q);
		CHECK(q[0].fg[0] < 0.01f && q[0].bg[0] == 0.0f, "faded colours %g %g", q[0].fg[0], q[0].bg[0]);
	}
	pal[7] = 0xf0f0f0;
	pal[12] = 0x90b0c0;

	/* the pointer over a corner of the first cell: a few pixels differ and are repainted (only if the
	 * pointer is known to be there) */
	g_ptr = 1;
	g_px = 96;
	g_py = 96;
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			frame[(100 + r) * SCREEN_W + 100 + c] = 0;
	{
		const struct tl_quad *q;
		tl_validate(frame, pal, g_ptr, g_px, g_py);
		struct tl_stats st;
		int n = tl_layout(2.0f, 2.0f, -1, &q);
		tl_get_stats(&st);
		CHECK(st.shown == 2 && st.repaint > 0 && n == 2 + st.repaint, "pointer: shown %d repaint %d quads %d", st.shown,
		      st.repaint, n);
	}

	g_ptr = 0;
	/* differences all over (a wipe): strict, no cell with a differing pixel is shown, none is repainted */
	{
		uint8_t keep[8][16];
		for (int r = 0; r < 4; r++)
			memcpy(keep[r], frame + (100 + r) * SCREEN_W + 100, 16);
		frame[(104) * SCREEN_W + 103] ^= 1;      /* a pixel of the first cell ... */
		frame[(104) * SCREEN_W + 116 + 3] ^= 1;  /* ... and one of the second, 16 columns apart is still one box */
		draw(100, 200, 0x4e01, 7, 12);
		frame[204 * SCREEN_W + 103] ^= 1;        /* a third, in a cell 100 rows down: no longer one box */
		const struct tl_quad *q;
		tl_validate(frame, pal, g_ptr, g_px, g_py);
		struct tl_stats st;
		tl_layout(2.0f, 2.0f, -1, &q);
		tl_get_stats(&st);
		CHECK(st.repaint == 0, "spread differences are not repainted: %d", st.repaint);
		tl_fill(SURF_PRIMARY_HANDLE, 100, 200, 116, 216);
		frame[(104) * SCREEN_W + 103] ^= 1;
		frame[(104) * SCREEN_W + 116 + 3] ^= 1;
		for (int r = 0; r < 4; r++)
			CHECK(!memcmp(keep[r], frame + (100 + r) * SCREEN_W + 100, 16), "test restored the frame");
	}

	/* the first cell cleared to its background (no tracked call): not shown, the second still is */
	for (int r = 0; r < 16; r++)
		for (int c = 0; c < 16; c++)
			frame[(100 + r) * SCREEN_W + 100 + c] = 12;
	CHECK(shown() == 1, "cleared cell hidden: %d", shown());

	/* drawn over with other pixels */
	for (int r = 0; r < 16; r++)
		for (int c = 0; c < 16; c++)
			frame[(100 + r) * SCREEN_W + 116 + c] = (uint8_t)(r + c + 40);
	CHECK(shown() == 0, "covered cell hidden: %d", shown());

	/* it comes back when the pixels do (a restored background) */
	draw(100, 100, 0x6559, 7, 12);
	CHECK(shown() == 1, "redrawn: %d", shown());

	/* a new cell over an old one drops the old; a half-width cell does not touch its neighbour */
	draw(100, 100, 0x4e00, 7, 12);
	CHECK(shown() == 1, "replaced: %d", shown());
	{
		struct tl_glyph g = make_glyph('A');
		g.len = 1;
		g.cell_w = 8;
		tl_glyph_drawn(&g);
		for (int r = 0; r < 16; r++)
			for (int c = 0; c < 16; c++)
				frame[(140 + r) * SCREEN_W + 200 + c] = (uint8_t)(c < 8 && (g.mask[r] >> c & 1) ? 7 : 12);
		tl_cell_placed(SURF_PRIMARY_HANDLE, 200, 140, 16, 7, 12);
		draw(208, 140, 'B', 7, 12); /* the next character's cell over the right half */
		CHECK(shown() == 3, "half-width then next: %d", shown());
	}

	/* a fill over a cell: tracking drops it, even though the pixels were not changed here */
	tl_fill(SURF_PRIMARY_HANDLE, 90, 90, 130, 130);
	CHECK(shown() == 2, "fill drops the entry: %d", shown());

	/* BltFast of a rectangle holding a cell: it follows to the destination */
	{
		draw(300, 300, 0x6559, 7, 12);
		for (int r = 0; r < 16; r++)
			memcpy(frame + (60 + r) * SCREEN_W + 400, frame + (300 + r) * SCREEN_W + 300, 16);
		tl_blit(SURF_PRIMARY_HANDLE, 400, 60, SURF_PRIMARY_HANDLE, 300, 300, 316, 316);
		CHECK(shown() == 4, "blit copies the entry: %d", shown());
		tl_fill(SURF_PRIMARY_HANDLE, 400, 60, 416, 76);
		CHECK(shown() == 3, "and a fill drops it: %d", shown());
	}

	/* the mode off: nothing */
	tl_set_mode(TEXT_BITMAP);
	{
		const struct tl_quad *q;
		CHECK(tl_layout(2.0f, 2.0f, -1, &q) == 0, "bitmap mode draws nothing");
	}

	/* a same-colour cell and an inkless one are not tracked */
	tl_set_mode(TEXT_HIRES);
	{
		int before = shown();
		draw(500, 400, 0x6559, 7, 7);
		struct tl_glyph g = make_glyph(' ');
		memset(g.mask, 0, sizeof g.mask);
		tl_glyph_drawn(&g);
		tl_cell_placed(SURF_PRIMARY_HANDLE, 520, 400, 16, 7, 12);
		CHECK(shown() == before, "fg == bg and no ink are skipped");
	}

	/* a look-alike character: a few pixels of a cell change (the game's raw copies rewrote the page).
	 * Without the pointer there, the cell is hidden; with the pointer over those pixels it is repainted */
	{
		tl_set_mode(TEXT_HIRES);
		tl_fill(SURF_PRIMARY_HANDLE, 0, 0, 640, 480);
		draw(300, 100, 0x672a, 7, 12);
		CHECK(shown() == 1, "fresh cell: %d", shown());
		frame[105 * SCREEN_W + 304] ^= 1;
		frame[106 * SCREEN_W + 305] ^= 1;
		g_ptr = 0;
		CHECK(shown() == 0, "changed pixels, no pointer: hidden (%d)", shown());
		g_ptr = 1;
		g_px = 400;
		g_py = 300;
		CHECK(shown() == 0, "changed pixels, pointer elsewhere: hidden (%d)", shown());
		g_px = 290;
		g_py = 95;
		CHECK(shown() == 1, "changed pixels under the pointer: shown (%d)", shown());
		g_ptr = 0;
		frame[105 * SCREEN_W + 304] ^= 1;
		frame[106 * SCREEN_W + 305] ^= 1;
		/* a 3 px band changed across two cells (a wipe edge), pointer far away */
		draw(316, 100, 0x672b, 7, 12);
		for (int r = 0; r < 16; r++)
			for (int c = 14; c < 18; c++)
				frame[(100 + r) * SCREEN_W + 300 + c] = 3;
		g_ptr = 1;
		g_px = 500;
		g_py = 400;
		CHECK(shown() == 0, "wipe band: both hidden (%d)", shown());
		g_ptr = 0;
	}

	/* a glyph the rasteriser has no outline for: the bitmap glyph stays (no quad for the cell) */
	{
		tl_fill(SURF_PRIMARY_HANDLE, 0, 0, 640, 480);
		draw(100, 100, 0x4e01, 7, 12);
		draw(116, 100, 0x4e02, 7, 12);
		g_empty = 1;
		const struct tl_quad *q;
		tl_validate(frame, pal, 0, 0, 0);
		int n = tl_layout(2.0f, 2.0f, -1, &q);
		CHECK(n == 0, "empty glyphs draw no cell: %d quads", n);
		g_empty = 0;
	}

	/* an atlas that cannot hold the screen's glyphs: one reset at most, the rest wait (bitmap), no
	 * rasterising and uploading every frame */
	{
		tl_fill(SURF_PRIMARY_HANDLE, 0, 0, 640, 480);
		g_big = 4;
		int cells = 0;
		for (int y = 0; y + 16 <= 400 && cells < 400; y += 16)
			for (int x = 0; x + 16 <= 640 && cells < 400; x += 16, cells++)
				draw(x, y, 0x5000 + (uint32_t)cells, 7, 12);
		struct tl_stats a, b;
		tl_validate(frame, pal, 0, 0, 0);
		const struct tl_quad *q;
		tl_layout(6.0f, 6.0f, -1, &q);
		tl_get_stats(&a);
		for (int f = 0; f < 20; f++) {
			tl_validate(frame, pal, 0, 0, 0);
			tl_layout(6.0f, 6.0f, -1, &q);
		}
		tl_get_stats(&b);
		CHECK(b.atlas_resets - a.atlas_resets <= 1, "atlas resets in 20 frames: %lu", b.atlas_resets - a.atlas_resets);
		CHECK(b.shown + b.waiting == cells, "shown %d + waiting %d = %d cells", b.shown, b.waiting, cells);
		CHECK(b.waiting > 0 && b.shown > 0, "partly shown: %d / %d", b.shown, b.waiting);
		CHECK(b.glyphs - a.glyphs < (unsigned long)cells, "no re-rasterising every frame: %lu", b.glyphs - a.glyphs);
		/* a full atlas is remembered: frames before the next reset rasterise nothing */
		int calls = g_calls;
		for (int f = 0; f < 10; f++) {
			tl_validate(frame, pal, 0, 0, 0);
			tl_layout(6.0f, 6.0f, -1, &q);
		}
		CHECK(g_calls == calls, "rasteriser calls while the atlas is full: %d", g_calls - calls);
		g_big = 1;
	}

	/* the cap: far more cells than fit */
	for (int i = 0; i < 6000; i++)
		draw(8 * (i % 70), 8 * (i / 70) % 440, 0x6559 + (uint32_t)i, 7, 12);
	{
		struct tl_stats st;
		tl_get_stats(&st);
		CHECK(st.entries <= 4096, "entries capped: %d", st.entries);
	}

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("textlayer test: ok\n");
	return 0;
}
