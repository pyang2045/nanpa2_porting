#ifndef NANPA2_LZH_H
#define NANPA2_LZH_H

#include <stddef.h>

/* Decompress a .GP4/.ANI stream. Returns a malloc'd buffer or NULL. */
unsigned char *lzh_decode(const unsigned char *src, size_t len, size_t *out_len);

#endif
