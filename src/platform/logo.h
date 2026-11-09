#ifndef NANPA2_LOGO_H
#define NANPA2_LOGO_H

#include <stdint.h>

/* Native 0x401e20: plays LOGO.AVI (Cinepak 640x480 30 fps, 8-bit mono
 * 22050 Hz PCM) until it ends or a key or mouse button is pressed. */
void logo_play(void);

/* Decode frame n (0-based) of LOGO.AVI into xrgb (640x480 0x00RRGGBB) for
 * checking the decoder headlessly; returns the frame count, 0 on error. */
int logo_frame(int n, uint32_t *xrgb);

#endif
