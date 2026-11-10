/* The high-resolution text layer: see textlayer.h. */
#include "platform/textlayer.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/clock.h"
#include "platform/surface.h"
#include "platform/text.h"

#define SCREEN_W 640 /* the game's screen */
#define SCREEN_H 480

static int popcount32(uint32_t v)
{
	int n = 0;
	for (; v; v &= v - 1)
		n++;
	return n;
}

static double ms_since(uint64_t t0_ns)
{
	return (double)(clock_host_ns() - t0_ns) / 1e6;
}

/* ---- mode ---- */

static int mode = TEXT_HIRES;
static void (*change_cb)(int);

void tl_set_mode(int m)
{
	if (m >= 0 && m < TEXT_MODES)
		mode = m;
}

int tl_get_mode(void)
{
	return mode;
}

void tl_toggle(void)
{
	mode = mode == TEXT_HIRES ? TEXT_BITMAP : TEXT_HIRES;
	if (change_cb)
		change_cb(mode);
}

const char *tl_mode_name(int m)
{
	return m == TEXT_HIRES ? "hires" : "bitmap";
}

int tl_parse_mode(const char *s)
{
	if (s && !strcmp(s, "hires"))
		return TEXT_HIRES;
	if (s && !strcmp(s, "bitmap"))
		return TEXT_BITMAP;
	return -1;
}

void tl_on_change(void (*cb)(int))
{
	change_cb = cb;
}

/* ---- tracking ---- */

#define MAX_ENT 4096
#define POINTER_SIZE 32 /* the pointer sprite's size (0x4012e0) */
#define POINTER_SLACK 8 /* the pointer moved a little between the message and its drawing */
#define MIN_INK 4 /* a cell with less ink is not worth showing (and would match by chance) */

struct entry {
	uint32_t surf;
	int x, y; /* the cell's top left on the surface */
	int n, w; /* n rows; w columns: the glyph's advance (the rest of a half-width cell is the next one's) */
	uint8_t fg, bg;
	int ink; /* pixels of ink in the visible columns */
	struct tl_glyph g;
};
static struct entry ents[MAX_ENT];
static int nent;
static struct tl_glyph last;
static int last_valid;
static unsigned long total_drawn, total_overlaps;

void tl_glyph_drawn(const struct tl_glyph *g)
{
	last = *g;
	last_valid = 1;
}

static int overlaps(const struct entry *e, uint32_t surf, int x0, int y0, int x1, int y1)
{
	return e->surf == surf && e->x < x1 && e->x + e->w > x0 && e->y < y1 && e->y + e->n > y0;
}

/* Drop the entries on surf that touch [x0, x1) x [y0, y1); returns how many. */
static int drop(uint32_t surf, int x0, int y0, int x1, int y1)
{
	int k = 0;
	for (int i = 0; i < nent; i++)
		if (!overlaps(&ents[i], surf, x0, y0, x1, y1)) {
			if (k != i)
				ents[k] = ents[i];
			k++;
		}
	int dropped = nent - k;
	nent = k;
	return dropped;
}

static struct entry *add_entry(void)
{
	if (nent >= MAX_ENT) { /* the oldest quarter goes */
		int cut = MAX_ENT / 4;
		memmove(ents, ents + cut, (size_t)(MAX_ENT - cut) * sizeof ents[0]);
		nent = MAX_ENT - cut;
	}
	return &ents[nent++];
}

void tl_cell_placed(uint32_t surface, int x, int y, int n, int fg, int bg)
{
	total_drawn++;
	if (!last_valid)
		return;
	last_valid = 0;
	int w = last.cell_w ? (last.len > 1 ? n : last.cell_w) : n;
	if (w > n)
		w = n;
	int hit = drop(surface, x, y, x + w, y + n);
	if (hit)
		total_overlaps += (unsigned long)hit;
	if (!last.n || last.n != n || fg == bg)
		return;
	int ink = 0;
	for (int r = 0; r < n; r++)
		ink += popcount32(last.mask[r] & (w >= 32 ? ~0u : (1u << w) - 1));
	if (ink < MIN_INK)
		return;
	struct entry *e = add_entry();
	e->surf = surface;
	e->x = x;
	e->y = y;
	e->n = n;
	e->w = w;
	e->fg = (uint8_t)fg;
	e->bg = (uint8_t)bg;
	e->ink = ink;
	e->g = last;
}

void tl_fill(uint32_t surface, int l, int t, int r, int b)
{
	if (nent && l < r && t < b)
		drop(surface, l, t, r, b);
}

void tl_blit(uint32_t dst, int x, int y, uint32_t src, int l, int t, int r, int b)
{
	if (!nent || l >= r || t >= b)
		return;
	int dw, dh;
	if (!surf_size(dst, &dw, &dh))
		return;
	/* the entries wholly inside the source rectangle, moved to where they land */
	enum { MAXMOVE = 512 };
	static struct entry moved[MAXMOVE];
	int nm = 0;
	for (int i = 0; i < nent && nm < MAXMOVE; i++) {
		const struct entry *e = &ents[i];
		if (e->surf != src || e->x < l || e->x + e->w > r || e->y < t || e->y + e->n > b)
			continue;
		int nx = e->x - l + x, ny = e->y - t + y;
		if (nx < 0 || ny < 0 || nx + e->w > dw || ny + e->n > dh)
			continue;
		moved[nm] = *e;
		moved[nm].surf = dst;
		moved[nm].x = nx;
		moved[nm].y = ny;
		nm++;
	}
	drop(dst, x, y, x + (r - l), y + (b - t));
	for (int i = 0; i < nm; i++)
		*add_entry() = moved[i];
}

/* ---- validation ---- */

struct vis {
	struct entry e; /* a copy: the entries change under the game while a frame waits to be drawn */
	int rep0, rep1; /* its repainted pixels in repaint[] */
	float fg[3], bg[3];
};
struct rep {
	int x, y;
	float c[3];
};
#define MAX_REP 8192
static struct vis vis[MAX_ENT];
static int nvis;
static struct rep repaint[MAX_REP];
static int nrep;
static float lin[256][3];
static int valid_count;
static double validate_ms, worst_validate_ms;

void tl_clear(void)
{
	nvis = nrep = 0;
	valid_count = 0;
}

static void publish_stats(void);

void tl_validate(const uint8_t *frame, const uint32_t pal[256], int have_ptr, int ptr_x, int ptr_y)
{
	uint64_t t0 = clock_host_ns();
	nvis = nrep = 0;
	if (nent) {
		for (int i = 0; i < 256; i++) {
			lin[i][0] = powf((float)((pal[i] >> 16) & 255) / 255.0f, 2.2f);
			lin[i][1] = powf((float)((pal[i] >> 8) & 255) / 255.0f, 2.2f);
			lin[i][2] = powf((float)(pal[i] & 255) / 255.0f, 2.2f);
		}
	}
	/* The pointer's sprite, where differences are forgiven (and a little around it). */
	int rx0 = ptr_x - POINTER_SLACK, ry0 = ptr_y - POINTER_SLACK, rx1 = ptr_x + POINTER_SIZE + POINTER_SLACK,
	    ry1 = ptr_y + POINTER_SIZE + POINTER_SLACK;
	for (int i = 0; i < nent; i++) {
		const struct entry *e = &ents[i];
		if (e->surf != SURF_PRIMARY_HANDLE || e->x < 0 || e->y < 0 || e->x + e->w > SCREEN_W ||
		    e->y + e->n > SCREEN_H)
			continue;
		/* A cell is shown only if it is on the screen as drawn. The pixels under the pointer may
		 * differ: they are repainted as they are, as long as the cell keeps 70% of its ink and half
		 * its area (with the ink gone the glyph's edges between repainted pixels would be a ghost).
		 * A difference anywhere else (a wipe, a page rewritten by the game's raw copies, even by a
		 * look-alike character) hides the cell. */
		int bad = 0, bad_ink = 0, bad_max = e->w * e->n / 2, bad_ink_max = e->ink * 3 / 10, rep0 = nrep, ok = 1;
		for (int r = 0; r < e->n && ok; r++) {
			const uint8_t *row = frame + (e->y + r) * SCREEN_W + e->x;
			uint32_t m = e->g.mask[r];
			for (int c = 0; c < e->w; c++) {
				int ink = m >> c & 1;
				if (row[c] == (ink ? e->fg : e->bg))
					continue;
				int px = e->x + c, py = e->y + r;
				bad++;
				bad_ink += ink;
				if (!have_ptr || px < rx0 || px >= rx1 || py < ry0 || py >= ry1 || bad > bad_max ||
				    bad_ink > bad_ink_max || nrep >= MAX_REP) {
					ok = 0;
					break;
				}
				repaint[nrep].x = px;
				repaint[nrep].y = py;
				memcpy(repaint[nrep].c, lin[row[c]], sizeof repaint[nrep].c);
				nrep++;
			}
		}
		if (!ok) {
			nrep = rep0;
			continue;
		}
		struct vis *v = &vis[nvis++];
		v->e = *e;
		v->rep0 = rep0;
		v->rep1 = nrep;
		memcpy(v->fg, lin[e->fg], sizeof v->fg);
		memcpy(v->bg, lin[e->bg], sizeof v->bg);
	}
	valid_count = nvis;
	validate_ms = ms_since(t0);
	if (validate_ms > worst_validate_ms)
		worst_validate_ms = validate_ms;
	publish_stats();
}

/* ---- the glyph cache and atlas ---- */

static uint8_t atlas[TL_ATLAS_W * TL_ATLAS_H];
static int pack_x, pack_y, pack_h, dirty_y0 = TL_ATLAS_H, dirty_y1;

struct slot {
	uint64_t key; /* 0: free */
	int ax, ay, w, h, xo, yo;
	float pen_x;
	int empty; /* nothing to draw */
};
#define NSLOT 16384 /* a power of two, twice the atlas's capacity for 16 px glyphs at least */
static struct slot slots[NSLOT];
static int nslots;
static unsigned long atlas_resets, glyph_count;
static unsigned long layouts, layouts_at_reset; /* tl_layout calls with something to draw; at the last reset */
#define RESET_GAP 120 /* frames between atlas resets: a working set that does not fit stays partly bitmap
		       * instead of being rasterised and uploaded again every frame */

/* bit 0 set (so no key is 0), then code point 21 bits, face 3, font 3, Big5 1, em 8, bold 1, scale 12 */
_Static_assert(TL_MAX_FONTS <= 8 && TL_MAX_FACES <= 8, "key_of: 3 bits each for font and face");

static uint64_t key_of(const struct tl_glyph *g, int sq)
{
	return 1ull | (uint64_t)(g->cp & 0x1fffff) << 1 | (uint64_t)(g->face & 7) << 22 | (uint64_t)(g->fi & 7) << 25 |
	       (uint64_t)(g->len > 1) << 28 | (uint64_t)g->em << 29 | (uint64_t)g->bold << 37 |
	       (uint64_t)(sq & 0xfff) << 38;
}

static struct slot *find_slot(uint64_t key, int create)
{
	uint32_t i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 50) & (NSLOT - 1);
	for (int probe = 0; probe < NSLOT; probe++, i = (i + 1) & (NSLOT - 1)) {
		if (slots[i].key == key)
			return &slots[i];
		if (!slots[i].key)
			return create ? &slots[i] : NULL;
	}
	return NULL;
}

static void atlas_reset(void)
{
	memset(slots, 0, sizeof slots);
	nslots = 0;
	pack_x = pack_y = pack_h = 0;
	atlas_resets++;
}

/* Room for a w x h bitmap: its top left in the atlas, 0 if it is full. */
static int atlas_alloc(int w, int h, int *ax, int *ay)
{
	if (w > TL_ATLAS_W || h > TL_ATLAS_H)
		return 0;
	if (pack_x + w > TL_ATLAS_W) {
		pack_y += pack_h;
		pack_x = pack_h = 0;
	}
	if (pack_y + h > TL_ATLAS_H)
		return 0;
	*ax = pack_x;
	*ay = pack_y;
	pack_x += w;
	if (h > pack_h)
		pack_h = h;
	return 1;
}

const uint8_t *tl_atlas(int *y0, int *y1)
{
	*y0 = dirty_y0;
	*y1 = dirty_y1;
	return atlas;
}

void tl_atlas_clean(void)
{
	dirty_y0 = TL_ATLAS_H;
	dirty_y1 = 0;
}

/* The slot of the glyph at sq/64 output pixels per game pixel, rasterised now if it is not there
 * (and the clock allows: *deadline is a performance-counter value, 0 for no limit). NULL: wait;
 * *full is set when the atlas has no room. */
static struct slot *glyph_slot(const struct tl_glyph *g, int sq, uint64_t deadline, int *full, double *raster_ms)
{
	uint64_t key = key_of(g, sq);
	struct slot *s = find_slot(key, 0);
	if (s)
		return s;
	if (deadline && clock_host_ns() > deadline)
		return NULL;
	uint64_t t0 = clock_host_ns();
	int w = 0, h = 0, xo = 0, yo = 0;
	float pen = 0;
	uint8_t *bm = text_hires_glyph(g, (float)sq / 64.0f, &w, &h, &xo, &yo, &pen);
	int ax = 0, ay = 0;
	if (bm && (w > TL_ATLAS_W || h > TL_ATLAS_H)) { /* too big for the atlas at all: stays bitmap */
		text_hires_free(bm);
		bm = NULL;
	}
	if (bm && !atlas_alloc(w, h, &ax, &ay)) {
		text_hires_free(bm);
		*full = 1;
		return NULL;
	}
	if (nslots >= NSLOT * 3 / 4) {
		if (bm)
			text_hires_free(bm);
		*full = 1;
		return NULL;
	}
	s = find_slot(key, 1);
	if (!s) {
		if (bm)
			text_hires_free(bm);
		*full = 1;
		return NULL;
	}
	nslots++;
	glyph_count++;
	s->key = key;
	s->empty = !bm;
	s->pen_x = pen;
	if (bm) {
		for (int y = 0; y < h; y++)
			memcpy(atlas + (ay + y) * TL_ATLAS_W + ax, bm + y * w, (size_t)w);
		s->ax = ax;
		s->ay = ay;
		s->w = w;
		s->h = h;
		s->xo = xo;
		s->yo = yo;
		if (ay < dirty_y0)
			dirty_y0 = ay;
		if (ay + h > dirty_y1)
			dirty_y1 = ay + h;
		text_hires_free(bm);
	}
	*raster_ms += ms_since(t0);
	return s;
}

/* ---- the quads ---- */

/* The outline's ink sits about a quarter game pixel higher than the bitmap glyph's (measured over the
 * shown cells, NANPA2_LOG_TEXT=2); the vector glyph is drawn that much lower. */
#define BASE_SHIFT 0.25f

static struct tl_quad *quads;
static int nquads, cap_quads;
static struct tl_stats st;
static double worst_layout_ms, layout_total_ms;
static unsigned long layout_calls;

static struct tl_quad *quad_add(int x0, int y0, int x1, int y1)
{
	if (x1 <= x0 || y1 <= y0)
		return NULL;
	if (nquads >= cap_quads) {
		int c = cap_quads ? cap_quads * 2 : 1024;
		struct tl_quad *q = realloc(quads, (size_t)c * sizeof *quads);
		if (!q)
			return NULL;
		quads = q;
		cap_quads = c;
	}
	struct tl_quad *q = &quads[nquads++];
	memset(q, 0, sizeof *q);
	q->x0 = (float)x0;
	q->y0 = (float)y0;
	q->x1 = (float)x1;
	q->y1 = (float)y1;
	return q;
}

static int rnd(float v)
{
	return (int)floorf(v + 0.5f);
}

/* NANPA2_LOG_TEXT=2: how well the vector glyph agrees with the game's bitmap one (ink area and
 * bounding box in game pixels, summed over the shown cells; tl_get_stats' caller prints them) */
static int diag = -1;
static struct {
	int n;
	double area_lo, area_hi, dx, dy, adx, ady, worst_dx, worst_dy;
} dg;

static void diagnose(const struct entry *e, const struct slot *s, float sx, float sy)
{
	int x0 = 99, x1 = -1, y0 = 99, y1 = -1, area = 0;
	for (int r = 0; r < e->n; r++)
		for (int c = 0; c < e->w; c++)
			if (e->g.mask[r] >> c & 1) {
				area++;
				if (c < x0) x0 = c;
				if (c > x1) x1 = c;
				if (r < y0) y0 = r;
				if (r > y1) y1 = r;
			}
	if (x1 < 0)
		return;
	double sum = 0;
	int hx0 = 1 << 30, hx1 = -1, hy0 = 1 << 30, hy1 = -1;
	for (int y = 0; y < s->h; y++)
		for (int x = 0; x < s->w; x++) {
			int v = atlas[(s->ay + y) * TL_ATLAS_W + s->ax + x];
			sum += v;
			if (v >= 128) {
				if (x < hx0) hx0 = x;
				if (x > hx1) hx1 = x;
				if (y < hy0) hy0 = y;
				if (y > hy1) hy1 = y;
			}
		}
	if (hx1 < 0)
		return;
	/* the vector glyph's box in game pixels from the cell's top left */
	double gx0 = ((double)rnd(((float)e->x + s->pen_x) * sx) + s->xo + hx0) / sx - e->x;
	double gx1 = ((double)rnd(((float)e->x + s->pen_x) * sx) + s->xo + hx1 + 1) / sx - e->x;
	double gy0 = ((double)rnd(((float)e->y + e->g.base) * sy) + s->yo + hy0) / sy - e->y;
	double gy1 = ((double)rnd(((float)e->y + e->g.base) * sy) + s->yo + hy1 + 1) / sy - e->y;
	double dx = (gx0 + gx1) / 2 - (x0 + x1 + 1) / 2.0, dy = (gy0 + gy1) / 2 - (y0 + y1 + 1) / 2.0;
	dg.n++;
	dg.area_lo += area;
	dg.area_hi += sum / 255.0 / (sx * sy);
	dg.dx += dx;
	dg.dy += dy;
	dg.adx += fabs(dx);
	dg.ady += fabs(dy);
	if (fabs(dx) > fabs(dg.worst_dx)) dg.worst_dx = dx;
	if (fabs(dy) > fabs(dg.worst_dy)) dg.worst_dy = dy;
}

void tl_print_diag(void)
{
	if (dg.n)
		fprintf(stderr, "textdiag: %d glyphs, ink area vector/bitmap %.2f, box centre offset (vector - bitmap, game px) "
			"mean %.2f,%.2f abs %.2f,%.2f worst %.2f,%.2f\n", dg.n, dg.area_hi / dg.area_lo, dg.dx / dg.n,
			dg.dy / dg.n, dg.adx / dg.n, dg.ady / dg.n, dg.worst_dx, dg.worst_dy);
	memset(&dg, 0, sizeof dg);
}

int tl_layout(float sx, float sy, int budget_us, const struct tl_quad **out)
{
	*out = NULL;
	st.shown = st.waiting = st.repaint = 0;
	st.layout_ms = st.raster_ms = 0;
	if (mode != TEXT_HIRES || sy < 1.5f || !nvis)
		return 0;
	if (diag < 0)
		diag = getenv("NANPA2_LOG_TEXT") && atoi(getenv("NANPA2_LOG_TEXT")) >= 2;
	uint64_t t0 = clock_host_ns();
	uint64_t deadline = budget_us < 0 ? 0 : t0 + (uint64_t)budget_us * 1000u;
	int sq = rnd(sy * 64.0f);
	if (sq > 4000)
		sq = 4000;
	layouts++;
	int reset_done = 0;
restart:
	nquads = 0;
	st.shown = st.waiting = st.repaint = 0;
	st.raster_ms = 0;
	for (int i = 0; i < nvis; i++) {
		const struct vis *v = &vis[i];
		const struct entry *e = &v->e;
		int full = 0;
		struct slot *s = glyph_slot(&e->g, sq, deadline, &full, &st.raster_ms);
		if (!s && full && !reset_done && layouts - layouts_at_reset >= RESET_GAP) {
			/* the atlas is full: start it again with the glyphs of this frame, once */
			atlas_reset();
			layouts_at_reset = layouts;
			reset_done = 1;
			goto restart;
		}
		if (!s) { /* not rasterised yet, or no room: the bitmap glyph shows this frame */
			st.waiting++;
			continue;
		}
		if (s->empty) /* the face has no outline for it: the bitmap glyph shows */
			continue;
		int cx0 = rnd((float)e->x * sx), cx1 = rnd((float)(e->x + e->w) * sx);
		int cy0 = rnd((float)e->y * sy), cy1 = rnd((float)(e->y + e->n) * sy);
		if (diag)
			diagnose(e, s, sx, sy);
		struct tl_quad *q = quad_add(cx0, cy0, cx1, cy1);
		if (!q)
			continue;
		memcpy(q->fg, v->fg, sizeof q->fg);
		memcpy(q->bg, v->bg, sizeof q->bg);
		q->gx = (float)(rnd(((float)e->x + s->pen_x) * sx) + s->xo);
		q->gy = (float)(rnd(((float)e->y + e->g.base + BASE_SHIFT) * sy) + s->yo);
		q->gw = (float)s->w;
		q->gh = (float)s->h;
		q->ax = (float)s->ax;
		q->ay = (float)s->ay;
		for (int k = v->rep0; k < v->rep1; k++) {
			const struct rep *r = &repaint[k];
			struct tl_quad *p = quad_add(rnd((float)r->x * sx), rnd((float)r->y * sy), rnd((float)(r->x + 1) * sx),
						     rnd((float)(r->y + 1) * sy));
			if (!p)
				continue;
			memcpy(p->fg, r->c, sizeof p->fg);
			memcpy(p->bg, r->c, sizeof p->bg);
			st.repaint++;
		}
		st.shown++;
	}
	st.layout_ms = ms_since(t0);
	if (nquads) {
		layout_calls++;
		layout_total_ms += st.layout_ms;
		if (st.layout_ms > worst_layout_ms)
			worst_layout_ms = st.layout_ms;
	}
	publish_stats();
	*out = quads;
	return nquads;
}

/* The counters for another thread (the status file's): the main thread composes and publishes them
 * under a sequence number, readers retry while it is odd or changed. */
static struct tl_stats pub;
static atomic_uint pub_seq;

static void publish_stats(void)
{
	struct tl_stats c = st;
	c.entries = nent;
	c.valid = valid_count;
	c.atlas_resets = atlas_resets;
	c.glyphs = glyph_count;
	c.validate_ms = validate_ms;
	c.worst_validate_ms = worst_validate_ms;
	c.worst_layout_ms = worst_layout_ms;
	c.layout_total_ms = layout_total_ms;
	c.layout_calls = layout_calls;
	c.total_drawn = total_drawn;
	c.total_overlaps = total_overlaps;
	atomic_fetch_add_explicit(&pub_seq, 1, memory_order_acq_rel);
	pub = c;
	atomic_fetch_add_explicit(&pub_seq, 1, memory_order_release);
}

void tl_get_stats(struct tl_stats *out)
{
	unsigned a, b;
	do {
		a = atomic_load_explicit(&pub_seq, memory_order_acquire);
		*out = pub;
		atomic_thread_fence(memory_order_acquire);
		b = atomic_load_explicit(&pub_seq, memory_order_relaxed);
	} while ((a & 1) || a != b);
}
