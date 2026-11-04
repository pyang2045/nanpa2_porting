/* DirectDraw for the lifted code: 8-bit surfaces in emulated memory, a
 * palette, BltFast with colour keys, GetDC for text. The primary surface is
 * what the window shows. */
#include "hle.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "dev.h"
#include "platform/present.h"

#define MAX_SURF 16
#define DDSD_CAPS 1
#define DDSCAPS_PRIMARYSURFACE 0x200
#define DDBLTFAST_SRCCOLORKEY 1
#define DDERR_UNSUPPORTED 0x80004001u

static struct surf {
	uint32_t obj;    /* COM object address */
	uint32_t pixels; /* emulated address */
	int w, h, pitch;
	int primary;
	uint32_t ckey_lo, ckey_hi;
	int has_ckey;
} surfs[MAX_SURF];
static int nsurf;
static uint32_t palette_obj; /* entries (PALETTEENTRY x256) at palette_obj + 4 */
static uint32_t screen_w = 640, screen_h = 480;

uint32_t ddraw_create(void)
{
	return com_new(IF_DDRAW, 24, 0);
}

static struct surf *surf_of(uint32_t obj)
{
	for (int i = 0; i < nsurf; i++)
		if (surfs[i].obj == obj)
			return &surfs[i];
	return NULL;
}

static struct surf *primary(void)
{
	for (int i = 0; i < nsurf; i++)
		if (surfs[i].primary)
			return &surfs[i];
	return NULL;
}

int ddraw_surface_info(uint32_t hdc, uint32_t *pixels, int *pitch, int *w, int *h)
{
	uint32_t i = hdc - 0x5000u;
	if (i >= (uint32_t)nsurf)
		return 0;
	*pixels = surfs[i].pixels;
	*pitch = surfs[i].pitch;
	*w = surfs[i].w;
	*h = surfs[i].h;
	return 1;
}

void ddraw_set_entries(uint32_t start, uint32_t count, uint32_t entries)
{
	if (!palette_obj)
		return;
	for (uint32_t i = 0; i < count && start + i < 256; i++)
		wr32(palette_obj + 4 + 4 * (start + i), rd32(entries + 4 * i));
}

void ddraw_present(int force)
{
	static uint32_t last;
	static uint8_t frame[SCREEN_W * SCREEN_H];
	static uint32_t pal[256];
	uint32_t now = SDL_GetTicks();
	if (!force && now - last < 16)
		return;
	struct surf *p = primary();
	if (!p)
		return;
	last = now;
	for (int y = 0; y < SCREEN_H && y < p->h; y++)
		memcpy(frame + y * SCREEN_W, x86_ptr(p->pixels + (uint32_t)(y * p->pitch), SCREEN_W),
		       SCREEN_W);
	for (int i = 0; i < 256; i++) {
		uint32_t e = palette_obj ? rd32(palette_obj + 4 + 4u * (uint32_t)i) : 0;
		/* PALETTEENTRY: red, green, blue, flags */
		pal[i] = ((e & 0xff) << 16) | (e & 0xff00) | ((e >> 16) & 0xff);
	}
	dev_frame(frame, pal);
	/* NANPA2_HEADLESS: the presenter keeps the frame in memory (no window,
	 * no GPU, see present_common.h) */
	present_frame(frame, pal);
}

/* ---- methods ---- */

static uint32_t create_surface(uint32_t desc, uint32_t out)
{
	if (nsurf >= MAX_SURF)
		return DDERR_UNSUPPORTED;
	struct surf *s = &surfs[nsurf];
	uint32_t flags = rd32(desc + 4), caps = rd32(desc + 0x68);
	s->primary = (caps & DDSCAPS_PRIMARYSURFACE) != 0;
	if (s->primary || !(flags & 6)) {
		s->w = (int)screen_w;
		s->h = (int)screen_h;
	} else {
		s->h = (int)rd32(desc + 8);
		s->w = (int)rd32(desc + 0xc);
	}
	s->pitch = (s->w + 3) & ~3;
	s->pixels = x86_malloc((uint32_t)(s->pitch * s->h));
	s->obj = com_new(IF_DDSURFACE, 33, 4);
	wr32(s->obj + 4, (uint32_t)nsurf);
	wr32(out, s->obj);
	nsurf++;
	return 0;
}

/* Emulated CPU time of a blit (see x86_throttle): DirectDraw on a 486 with
 * system-memory surfaces copies about 4 pixels per instruction-time plain
 * (rep movsd) and 1 per pixel with a source colour key (software). */
static void blit_cost(int w, int h, int keyed)
{
	if (w > 0 && h > 0)
		X86_COUNT((uint64_t)w * (uint64_t)h / (keyed ? 1u : 4u) + 200u);
}

static void blt_fast(struct surf *d, int x, int y, struct surf *s, uint32_t rect, uint32_t flags)
{
	int l = 0, t = 0, r = s->w, b = s->h;
	if (rect) {
		l = (int)rd32(rect);
		t = (int)rd32(rect + 4);
		r = (int)rd32(rect + 8);
		b = (int)rd32(rect + 12);
	}
	int key = (flags & DDBLTFAST_SRCCOLORKEY) && s->has_ckey;
	blit_cost(r - l, b - t, key);
	for (int yy = t; yy < b; yy++) {
		int dy = y + (yy - t);
		if (yy < 0 || yy >= s->h || dy < 0 || dy >= d->h)
			continue;
		for (int xx = l; xx < r; xx++) {
			int dx = x + (xx - l);
			if (xx < 0 || xx >= s->w || dx < 0 || dx >= d->w)
				continue;
			uint32_t v = rd8(s->pixels + (uint32_t)(yy * s->pitch + xx));
			if (key && v >= s->ckey_lo && v <= s->ckey_hi)
				continue;
			wr8(d->pixels + (uint32_t)(dy * d->pitch + dx), v);
		}
	}
}

static int surface_method(uint32_t m)
{
	uint32_t self = hle_arg(0);
	struct surf *s = surf_of(self);
	if (!s)
		x86_trap(0, "DirectDraw surface method on unknown object");
	uint32_t r = 0;
	int nargs;
	switch (m) {
	case 1: /* AddRef */
	case 2: /* Release */
		nargs = 1;
		r = 1;
		break;
	case 5: { /* Blt(dstrect, src, srcrect, flags, fx) */
		uint32_t dr = hle_arg(1), flags = hle_arg(4), fx = hle_arg(5);
		int l = 0, t = 0, r = s->w, b = s->h;
		if (dr) {
			l = (int)rd32(dr); t = (int)rd32(dr + 4);
			r = (int)rd32(dr + 8); b = (int)rd32(dr + 12);
		}
		if (flags & 0x400) { /* DDBLT_COLORFILL */
			uint32_t c = rd32(fx + 0x50) & 0xff;
			blit_cost(r - l, b - t, 0);
			for (int y = t < 0 ? 0 : t; y < b && y < s->h; y++)
				for (int x = l < 0 ? 0 : l; x < r && x < s->w; x++)
					wr8(s->pixels + (uint32_t)(y * s->pitch + x), c);
		} else if (surf_of(hle_arg(2))) {
			struct surf *src = surf_of(hle_arg(2));
			uint32_t sr = hle_arg(3);
			int sl = 0, st = 0, srr = src->w, sb = src->h;
			if (sr) {
				sl = (int)rd32(sr); st = (int)rd32(sr + 4);
				srr = (int)rd32(sr + 8); sb = (int)rd32(sr + 12);
			}
			int key = (flags & 0x8000) && src->has_ckey; /* DDBLT_KEYSRC */
			blit_cost(r - l, b - t, key);
			for (int y = t; y < b; y++)
				for (int x = l; x < r; x++) {
					int sx = sl + (x - l) * (srr - sl) / (r - l);
					int sy = st + (y - t) * (sb - st) / (b - t);
					if (x < 0 || y < 0 || x >= s->w || y >= s->h || sx < 0 || sy < 0 ||
					    sx >= src->w || sy >= src->h)
						continue;
					uint32_t v = rd8(src->pixels + (uint32_t)(sy * src->pitch + sx));
					if (key && v >= src->ckey_lo && v <= src->ckey_hi)
						continue;
					wr8(s->pixels + (uint32_t)(y * s->pitch + x), v);
				}
		}
		nargs = 6;
		break;
	}
	case 7: { /* BltFast(x, y, src, rect, flags) */
		struct surf *src = surf_of(hle_arg(3));
		if (src)
			blt_fast(s, (int)hle_arg(1), (int)hle_arg(2), src, hle_arg(4), hle_arg(5));
		nargs = 6;
		break;
	}
	case 17: { /* GetDC(&hdc) */
		wr32(hle_arg(1), 0x5000u + (uint32_t)(s - surfs));
		nargs = 2;
		break;
	}
	case 22: /* GetSurfaceDesc(desc) */
	case 25: { /* Lock(rect, desc, flags, event) */
		uint32_t desc = m == 25 ? hle_arg(2) : hle_arg(1);
		wr32(desc + 4, 0x100f);
		wr32(desc + 8, (uint32_t)s->h);
		wr32(desc + 0xc, (uint32_t)s->w);
		wr32(desc + 0x10, (uint32_t)s->pitch);
		uint32_t px = s->pixels;
		if (m == 25 && hle_arg(1))
			px += rd32(hle_arg(1) + 4) * (uint32_t)s->pitch + rd32(hle_arg(1));
		wr32(desc + 0x24, px);
		nargs = m == 25 ? 5 : 2;
		break;
	}
	case 24: /* IsLost */
	case 27: /* Restore */
		nargs = 1;
		break;
	case 26: /* ReleaseDC(hdc) */
	case 31: /* SetPalette(pal) */
	case 32: /* Unlock(rect) */
		nargs = 2;
		break;
	case 29: { /* SetColorKey(flags, key) */
		if (hle_arg(2)) {
			s->ckey_lo = rd32(hle_arg(2));
			s->ckey_hi = rd32(hle_arg(2) + 4);
			s->has_ckey = 1;
		} else {
			s->has_ckey = 0;
		}
		nargs = 3;
		break;
	}
	case 11: /* Flip(surface, flags) */
		ddraw_present(1);
		nargs = 3;
		break;
	default:
		return 0;
	}
	hle_stdcall_pop(nargs);
	cpu.r[EAX] = r;
	return 1;
}

static int ddraw_method(uint32_t m)
{
	uint32_t r = 0;
	int nargs;
	switch (m) {
	case 1:
	case 2: /* AddRef/Release */
		nargs = 1;
		r = 1;
		break;
	case 5: { /* CreatePalette(flags, entries, &pal, outer) */
		palette_obj = com_new(IF_DDPALETTE, 7, 1024);
		for (uint32_t i = 0; i < 256; i++)
			wr32(palette_obj + 4 + 4 * i, rd32(hle_arg(2) + 4 * i));
		wr32(hle_arg(3), palette_obj);
		nargs = 5;
		break;
	}
	case 6: /* CreateSurface(desc, &surf, outer) */
		r = create_surface(hle_arg(1), hle_arg(2));
		nargs = 4;
		break;
	case 20: /* SetCooperativeLevel(hwnd, flags) */
		nargs = 3;
		break;
	case 21: /* SetDisplayMode(w, h, bpp) */
		screen_w = hle_arg(1);
		screen_h = hle_arg(2);
		nargs = 4;
		break;
	case 22: /* WaitForVerticalBlank(flags, event) */
		win32_run_timer();
		ddraw_present(0);
		nargs = 3;
		break;
	default:
		return 0;
	}
	hle_stdcall_pop(nargs);
	cpu.r[EAX] = r;
	return 1;
}

static int palette_method(uint32_t m)
{
	uint32_t self = hle_arg(0);
	int nargs;
	switch (m) {
	case 1:
	case 2:
		nargs = 1;
		break;
	case 4: /* GetEntries(flags, start, count, out) */
		for (uint32_t i = 0; i < hle_arg(3); i++)
			wr32(hle_arg(4) + 4 * i, rd32(self + 4 + 4 * ((hle_arg(2) + i) & 255)));
		nargs = 5;
		break;
	case 6: /* SetEntries(flags, start, count, entries) */
		if (self == palette_obj)
			ddraw_set_entries(hle_arg(2), hle_arg(3), hle_arg(4));
		nargs = 5;
		break;
	default:
		return 0;
	}
	hle_stdcall_pop(nargs);
	cpu.r[EAX] = 0;
	return 1;
}

int ddraw_com_call(uint32_t iface, uint32_t method)
{
	switch (iface) {
	case IF_DDRAW:
		return ddraw_method(method);
	case IF_DDSURFACE:
		return surface_method(method);
	case IF_DDPALETTE:
		return palette_method(method);
	}
	return 0;
}
