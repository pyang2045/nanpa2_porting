#ifndef NANPA2_GP4_H
#define NANPA2_GP4_H

#include <stdint.h>

/* A decoded .GP4: an 8-bit picture with its own 256-colour palette.
 * Decompressed layout: u16 x, y, w, h; u16 pal16[16] (4-bit 0x0GRB);
 * u16 pal240[240] (RGB555, palette entries 16..255); u8 pixels[w*h]. */
struct gp4 {
	int x, y, w, h;
	uint16_t pal_raw[256];
	uint8_t *pixels;
};

int gp4_load(const char *name, struct gp4 *out);
void gp4_free(struct gp4 *g);

/* Native 0x401ca0: raw palette words to 0x00RRGGBB. */
void gp4_palette_rgb(const uint16_t raw[256], uint32_t rgb[256]);

#endif
