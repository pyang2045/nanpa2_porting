#ifndef NANPA2_CINEPAK_H
#define NANPA2_CINEPAK_H

#include <stddef.h>
#include <stdint.h>

/* Cinepak ('cvid') video decoder, as used by LOGO.AVI (640x480, 30 fps).
 * Frames decode into a persistent 0x00RRGGBB buffer, since inter frames
 * only update some blocks. */

#define CVID_MAX_STRIPS 32

struct cvid_cb {
	uint8_t y[4];
	int8_t u, v;
};

struct cvid {
	int w, h;
	uint32_t *pixels; /* w * h */
	struct cvid_cb v1[CVID_MAX_STRIPS][256], v4[CVID_MAX_STRIPS][256];
};

int cvid_init(struct cvid *c, int w, int h);
void cvid_free(struct cvid *c);
/* Decode one frame chunk; returns 0 on malformed data (what was decoded
 * up to that point stays). */
int cvid_decode(struct cvid *c, const uint8_t *data, size_t size);

#endif
