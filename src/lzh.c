/*
 * Decoder for the compressed .GP4/.ANI files (native 0x4075f0, 0x40767c,
 * 0x4076cb, 0x40776b).
 *
 * LZHUF (adaptive Huffman + LZSS) with these differences from the classic
 * Okumura/Yoshizaki code:
 *   - bits are read LSB-first,
 *   - N_CHAR = 321: 256 literals and match lengths 3..67 (code - 253),
 *   - a match position is a prefix-coded upper 6 bits (code table below,
 *     native 0x4a72c0/0x4a73c0) followed by 6 raw low bits; the distance is
 *     position + 1 and position 0xfff ends the stream,
 *   - no ring buffer: matches copy from the output already written,
 *   - frequency rebuild (0x40767c) re-runs update() for every leaf whose
 *     parent's count is odd, then halves every count.
 */
#include "lzh.h"

#include <stdlib.h>
#include <string.h>

#define N_CHAR 321
#define T (2 * N_CHAR - 1)
#define R (T - 1)
#define MAX_FREQ 0x8000

static const unsigned char p_len[64] = {
	3, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
};

/* Low p_len[u] bits of the next stream byte that select upper bits u. */
static const unsigned char p_bits[64] = {
	7, 13, 11, 3, 30, 25, 21, 17, 14, 9, 5, 1,
	58, 54, 50, 42, 38, 34, 26, 22, 18, 10, 6, 2,
	124, 120, 116, 108, 104, 100, 92, 88, 84, 76, 72, 68,
	60, 56, 52, 44, 40, 36, 28, 24, 20, 12, 8, 4,
	240, 224, 208, 192, 176, 160, 144, 128,
	112, 96, 80, 64, 48, 32, 16, 0,
};

static unsigned char d_code[256], d_len[256];

static void build_tables(void)
{
	static int built;
	if (built)
		return;
	for (int u = 0; u < 64; u++) {
		int l = p_len[u];
		for (int hi = 0; hi < (1 << (8 - l)); hi++) {
			int b = p_bits[u] | (hi << l);
			d_code[b] = (unsigned char)u;
			d_len[b] = (unsigned char)l;
		}
	}
	built = 1;
}

struct huff {
	unsigned freq[T + 1];
	int son[T];
	int prnt[T + N_CHAR];
};

static void huff_init(struct huff *h)
{
	int i, j;
	for (i = 0; i < N_CHAR; i++) {
		h->freq[i] = 1;
		h->son[i] = i + T;
		h->prnt[i + T] = i;
	}
	for (i = 0, j = N_CHAR; j <= R; i += 2, j++) {
		h->freq[j] = h->freq[i] + h->freq[i + 1];
		h->son[j] = i;
		h->prnt[i] = h->prnt[i + 1] = j;
	}
	h->freq[T] = 0xffff;
}

/* native 0x4076cb: c is the parent of the leaf just decoded */
static void huff_update(struct huff *h, int c)
{
	do {
		unsigned k = ++h->freq[c];
		if (k > h->freq[c + 1]) {
			int l = c + 1;
			while (h->freq[l + 1] == k - 1)
				l++;
			h->freq[c] = h->freq[l];
			h->freq[l] = k;

			int i = h->son[c];
			h->prnt[i] = l;
			if (i < T)
				h->prnt[i + 1] = l;
			int j = h->son[l];
			h->son[l] = i;
			h->prnt[j] = c;
			if (j < T)
				h->prnt[j + 1] = c;
			h->son[c] = j;
			c = l;
		}
		c = h->prnt[c];
	} while (c != 0);
}

/* native 0x40767c */
static void huff_reconst(struct huff *h)
{
	for (int c = 0; c < N_CHAR; c++)
		if (h->freq[h->prnt[c + T]] & 1)
			huff_update(h, h->prnt[c + T]);
	for (int n = 0; n < T; n++)
		h->freq[n] >>= 1;
}

static int getbit(const unsigned char *src, size_t len, size_t bit)
{
	size_t i = bit >> 3;
	return i < len ? (src[i] >> (bit & 7)) & 1 : 0;
}

static unsigned peek32(const unsigned char *src, size_t len, size_t bit)
{
	size_t i = bit >> 3;
	unsigned v = 0;
	for (int k = 0; k < 4; k++)
		if (i + k < len)
			v |= (unsigned)src[i + k] << (8 * k);
	return v >> (bit & 7);
}

unsigned char *lzh_decode(const unsigned char *src, size_t len, size_t *out_len)
{
	build_tables();
	struct huff *h = calloc(1, sizeof *h); /* prnt[R] must be 0 */
	size_t cap = 1 << 16, n = 0, bit = 0;
	unsigned char *out = malloc(cap);
	if (!h || !out)
		goto fail;
	huff_init(h);

	for (;;) {
		if (bit > len * 8 + 64)
			goto fail; /* ran off the end: corrupt input */
		int c = h->son[R];
		while (c < T)
			c = h->son[c + getbit(src, len, bit++)];
		c -= T;
		if (h->freq[R] == MAX_FREQ)
			huff_reconst(h);
		huff_update(h, h->prnt[c + T]);

		if (n + 128 > cap) {
			cap *= 2;
			unsigned char *p = realloc(out, cap);
			if (!p)
				goto fail;
			out = p;
		}
		if (c < 256) {
			out[n++] = (unsigned char)c;
			continue;
		}
		unsigned v = peek32(src, len, bit);
		unsigned b = v & 0xff;
		unsigned l = d_len[b];
		unsigned pos = ((unsigned)d_code[b] << 6) | ((v >> l) & 0x3f);
		bit += l + 6;
		if (pos == 0xfff)
			break;
		size_t cnt = (size_t)(c - 253);
		if (pos + 1 > n)
			goto fail;
		size_t s = n - 1 - pos;
		for (size_t k = 0; k < cnt; k++)
			out[n++] = out[s + k];
	}
	free(h);
	*out_len = n;
	return out;
fail:
	free(h);
	free(out);
	return NULL;
}
