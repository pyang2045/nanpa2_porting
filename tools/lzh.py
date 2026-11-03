#!/usr/bin/env python3
"""Decoder for nanpa2.exe's compressed .GP4/.ANI files (native 0x4075f0).

LZHUF (Okumura/Yoshizaki adaptive Huffman + LZSS) with:
  - bits read LSB-first from the byte stream,
  - N_CHAR = 321 (256 literals + lengths 3..67), T = 641 nodes,
  - match position: 8-bit LSB-first peek -> (upper6, prefix length) from a
    fixed prefix table, then 6 raw low bits; distance = pos + 1,
  - no ring buffer: copies from already-written output,
  - position 0xfff ends the stream.
"""
import sys

N_CHAR = 321
T = 2 * N_CHAR - 1
R = T - 1
MAXFREQ = 0x8000

# Prefix codes for the upper 6 position bits, as the low P_LEN[u] bits of the
# next stream byte (LSB-first). Read from the native tables at 0x4a72c0
# (upper bits) and 0x4a73c0 (code length).
P_LEN = [3] + [4] * 3 + [5] * 8 + [6] * 12 + [7] * 24 + [8] * 16
P_BITS = [7, 13, 11, 3, 30, 25, 21, 17, 14, 9, 5, 1,
          58, 54, 50, 42, 38, 34, 26, 22, 18, 10, 6, 2,
          124, 120, 116, 108, 104, 100, 92, 88, 84, 76, 72, 68,
          60, 56, 52, 44, 40, 36, 28, 24, 20, 12, 8, 4,
          240, 224, 208, 192, 176, 160, 144, 128,
          112, 96, 80, 64, 48, 32, 16, 0]

def build_tables():
    """d_code/d_len indexed by the next 8 stream bits (LSB-first)."""
    d_code = [0] * 256
    d_len = [0] * 256
    for u in range(64):
        l = P_LEN[u]
        for hi in range(1 << (8 - l)):
            b = P_BITS[u] | (hi << l)
            d_code[b] = u; d_len[b] = l
    return d_code, d_len

D_CODE, D_LEN = build_tables()

def decode(src):
    freq = [1] * N_CHAR + [0] * (T - N_CHAR) + [0xffff]
    son = [0] * T
    prnt = [0] * (T + N_CHAR)
    for i in range(N_CHAR):
        son[i] = i + T
        prnt[i + T] = i
    i, j = 0, N_CHAR
    while j <= R:
        freq[j] = freq[i] + freq[i + 1]
        son[j] = i
        prnt[i] = prnt[i + 1] = j
        i += 2; j += 1

    def update(c):  # native 0x4076cb, entered with c = leaf parent
        while True:
            freq[c] += 1
            k = freq[c]
            if k > freq[c + 1]:
                l = c + 1
                while freq[l + 1] == k - 1:
                    l += 1
                freq[c] = freq[l]; freq[l] = k
                i = son[c]
                prnt[i] = l
                if i < T: prnt[i + 1] = l
                j = son[l]; son[l] = i
                prnt[j] = c
                if j < T: prnt[j + 1] = c
                son[c] = j
                c = l
            c = prnt[c]
            if c == 0:
                return

    def reconst():  # native 0x40767c (differs from standard LZHUF)
        for c in range(N_CHAR):
            if freq[prnt[c + T]] & 1:
                update(prnt[c + T])
        for n in range(T):
            freq[n] >>= 1

    data = bytes(src) + b'\0' * 8
    bit = 0
    out = bytearray()
    while True:
        c = son[R]
        while c < T:
            c = son[c + ((data[bit >> 3] >> (bit & 7)) & 1)]
            bit += 1
        c -= T
        if freq[R] == MAXFREQ:
            reconst()
        update(prnt[c + T])
        if c < 256:
            out.append(c)
            continue
        v = int.from_bytes(data[bit >> 3: (bit >> 3) + 4], 'little') >> (bit & 7)
        b = v & 0xff
        l = D_LEN[b]
        pos = (D_CODE[b] << 6) | ((v >> l) & 0x3f)
        bit += l + 6
        if pos == 0xfff:
            return bytes(out)
        n = c - 253
        s = len(out) - 1 - pos
        for _ in range(n):
            out.append(out[s]); s += 1

if __name__ == '__main__':
    d = decode(open(sys.argv[1], 'rb').read())
    if len(sys.argv) > 2:
        open(sys.argv[2], 'wb').write(d)
    print(len(d))
