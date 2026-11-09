/* The checkpoint hash (hash.h). */
#include "hash.h"

#include <string.h>

uint64_t hash_bytes(const void *p, size_t n, uint64_t h)
{
	const uint8_t *b = p;
	h ^= 0x9E3779B97F4A7C15ull + n;
	while (n >= 8) {
		uint64_t v;
		memcpy(&v, b, 8);
		h = (h ^ (v * 0xC2B2AE3D27D4EB4Full));
		h = ((h << 31) | (h >> 33)) * 0x9E3779B185EBCA87ull;
		b += 8;
		n -= 8;
	}
	uint64_t v = 0;
	memcpy(&v, b, n);
	h = (h ^ (v * 0xC2B2AE3D27D4EB4Full));
	h = ((h << 31) | (h >> 33)) * 0x9E3779B185EBCA87ull;
	h ^= h >> 29;
	return h;
}
