/*
 * Platform API: the high-resolution text layer.
 *
 * The game draws text as the 2000 original did: one 1-bit glyph at a time into an N x N cell
 * of the 8-bit 640x480 screen (src/game/text.c, native 0x401900, platform/text.c), and the
 * presenter then scales that screen. At the display's real resolution those 1-pixel strokes are
 * smeared. This layer keeps a record of the cells the game drew and, per presented frame,
 * shows an antialiased vector glyph over each cell that is still exactly on the screen as it
 * was drawn; everything else shows the picture as it is.
 *
 * It never touches the screen surface, G or the saves; it only reads the frame the presenter
 * is given. Visibility is decided by validation (tl_validate): a cell is shown only if the
 * frame's pixels still are the ones the glyph drew (ink = fg index, the rest = bg index, the
 * opaque cell of 0x401900). A cell that was drawn over, scrolled or wiped simply fails and the
 * low-resolution pixels show through; palette changes change colours, not indices, so the text
 * fades with the picture. Tracking (tl_cell_placed, tl_fill, tl_blit) only keeps the list short
 * and lets entries follow BltFast; a miss costs nothing but the hi-res look.
 *
 * The presenter asks for quads at its raster's scale (tl_layout) and draws them opaque, in linear
 * light: each cell in the colour mix(background, foreground, glyph coverage), then single pixels where
 * something small (the game cursor, a selection frame) was drawn over a shown cell.
 */
#ifndef NANPA2_PLATFORM_TEXTLAYER_H
#define NANPA2_PLATFORM_TEXTLAYER_H

#include <stdint.h>

/* Hi-res is always on for players; TEXT_BITMAP is a developer/test switch (--text bitmap) */
enum { TEXT_BITMAP, TEXT_HIRES, TEXT_MODES };
#define TL_MAX_FONTS 8 /* font numbers (text_font_create) and faces the layer can key */
#define TL_MAX_FACES 8
void tl_set_mode(int mode);
int tl_get_mode(void);
const char *tl_mode_name(int mode);
int tl_parse_mode(const char *s); /* hires|bitmap, or -1 */

/* ---- tracking (platform/text.c, platform/surface.c) ---- */

/* One character as text_draw rendered it into the scratch cell. */
struct tl_glyph {
	uint32_t cp;      /* Unicode code point */
	uint8_t face;     /* index of the font face it came from (text.c) */
	uint8_t fi;       /* font number (size and weight) */
	uint8_t len;      /* 1 for ASCII, 2 for Big5 */
	uint8_t em;       /* pixel size to draw the outline at: the strike's ppem or the font's em */
	uint8_t bold;     /* GDI emboldening: copied one pixel to the right */
	uint8_t n;        /* the cell is n x n pixels (16, 24, 32), 0: nothing was drawn */
	uint8_t cell_w;   /* the glyph's advance: em for Big5, half of it for ASCII */
	float base;       /* baseline row from the cell's top, in game pixels */
	float cx;         /* where the glyph's advance box was centred, from the cell's left, in game pixels */
	uint32_t mask[32]; /* ink: bit c of row r is set when pixel (c, r) is the foreground */
};
void tl_glyph_drawn(const struct tl_glyph *g);
/* The game copied the last glyph's cell to (x, y) of the surface with the colour indices fg, bg. */
void tl_cell_placed(uint32_t surface, int x, int y, int n, int fg, int bg);
/* The surface's rectangle [l, r) x [t, b) was overwritten, or copied (BltFast, same arguments). */
void tl_fill(uint32_t surface, int l, int t, int r, int b);
void tl_blit(uint32_t dst, int x, int y, uint32_t src, int l, int t, int r, int b);

/* ---- presenter ---- */

/* Check the entries against the frame the presenter was given (640x480 indices) and its palette
 * (0x00RRGGBB); selects what is shown. A cell must match exactly, except for pixels that lie inside the
 * game pointer's 32x32 sprite at (ptr_x, ptr_y) (have_ptr: the pointer's position is known): those are
 * repainted as they are, if the cell keeps most of its ink. tl_clear: nothing is shown (a true-colour
 * frame). */
void tl_validate(const uint8_t *frame, const uint32_t pal[256], int have_ptr, int ptr_x, int ptr_y);
void tl_clear(void);

/* One cell to draw, opaque: the rectangle [x0, x1) x [y0, y1) of the raster gets, for each pixel,
 * the colour mix(bg, fg, coverage), where the coverage is the atlas's at texel (ax, ay) + (pixel -
 * (gx, gy)) inside the glyph rectangle of gw x gh pixels at (gx, gy), and 0 outside it. Colours are
 * linear light. A single repainted pixel is a cell with gw = 0 and fg = bg. Five float4 for the GPU. */
struct tl_quad {
	float x0, y0, x1, y1;
	float gx, gy, gw, gh;
	float ax, ay, pad0, pad1;
	float fg[3], pad2;
	float bg[3], pad3;
};

/* The quads for the shown entries at sx x sy raster pixels per game pixel, in drawing order (a later
 * one covers an earlier one). Glyphs not rasterised yet cost time: with budget_us >= 0 only that much is
 * spent and the entries whose glyph is not ready stay low-resolution this frame (budget_us < 0: no
 * limit). Returns the quad count (0: draw nothing; the layer is off, or sy < 1.5), the quads valid until
 * the next call. */
int tl_layout(float sx, float sy, int budget_us, const struct tl_quad **out);

/* The glyph atlas (8-bit coverage) and the rows [*y0, *y1) that changed since the last
 * tl_atlas_clean (*y0 >= *y1: none). The atlas is cleared (all positions change) when it fills. */
#define TL_ATLAS_W 4096
#define TL_ATLAS_H 2048
const uint8_t *tl_atlas(int *y0, int *y1);
void tl_atlas_clean(void);

struct tl_stats {
	int entries;       /* tracked cells */
	int valid;         /* of them, still on the screen as drawn (last tl_validate) */
	int shown;         /* drawn by the last tl_layout */
	int waiting;       /* left low-resolution by the last tl_layout (glyph not rasterised yet, atlas full) */
	int repaint;       /* single pixels repainted over shown cells */
	unsigned long atlas_resets;
	unsigned long glyphs;     /* glyphs rasterised so far */
	double raster_ms;         /* time rasterising in the last tl_layout */
	double layout_ms;         /* time of the last tl_layout, rasterising included */
	double validate_ms;       /* time of the last tl_validate */
	double worst_layout_ms, worst_validate_ms; /* longest of each so far */
	double layout_total_ms;   /* summed over layout_calls (drawing frames), for the mean */
	unsigned long layout_calls;
	unsigned long total_drawn; /* cells the game drew */
	unsigned long total_overlaps; /* ... that partly covered an earlier cell */
};
/* The counters as the main thread last published them (validate, layout): safe to call from another thread. */
void tl_get_stats(struct tl_stats *out);
/* NANPA2_LOG_TEXT=2: print and reset how well the vector glyphs agree with the bitmap ones */
void tl_print_diag(void);

#endif
