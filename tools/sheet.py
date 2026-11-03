#!/usr/bin/env python3
"""Contact sheet of screenshots: sheet.py OUT.png COLS IN.bmp... (half size)"""
import struct, sys, zlib

def load_bmp(p):
    d = open(p, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bpp = struct.unpack_from('<H', d, 28)[0] // 8
    stride = (w * bpp + 3) & ~3
    rows = []
    for y in range(abs(h)):
        sy = abs(h) - 1 - y if h > 0 else y
        r = d[off + sy * stride: off + sy * stride + w * bpp]
        rows.append(bytes(b for x in range(w) for b in (r[x*bpp+2], r[x*bpp+1], r[x*bpp])))
    return w, abs(h), rows

def main():
    out, cols, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    imgs = [load_bmp(f) for f in files]
    w, h = imgs[0][0] // 2, imgs[0][1] // 2
    nrows = (len(imgs) + cols - 1) // cols
    raw = bytearray()
    for gy in range(nrows):
        for y in range(h):
            raw.append(0)
            for gx in range(cols):
                i = gy * cols + gx
                if i < len(imgs):
                    row = imgs[i][2][y * 2]
                    raw += b''.join(row[x * 6:x * 6 + 3] for x in range(w))
                else:
                    raw += b'\0' * (3 * w)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * cols, h * nrows, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b'')
    open(out, 'wb').write(png)

main()
