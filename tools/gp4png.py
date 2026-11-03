#!/usr/bin/env python3
"""Convert a .GP4 to PNG. usage: gp4png.py IN.GP4 OUT.png
Decoded layout: u16 x, y, w, h; u16 pal16[16] (0x0GRB 4-bit, native 0x401ca0);
u16 pal240[240] (RGB555, palette entries 16..255); u8 pixels[w*h]."""
import struct, sys, zlib, importlib.util, os

spec = importlib.util.spec_from_file_location('lzh', os.path.join(os.path.dirname(__file__), 'lzh.py'))
lzh = importlib.util.module_from_spec(spec); spec.loader.exec_module(lzh)

def palette(b):
    v = struct.unpack_from('<256H', b, 8)
    pal = []
    for i, c in enumerate(v):
        if i < 16:
            pal.append(((c & 0xf0), (c >> 4) & 0xf0, (c << 4) & 0xf0))
        else:
            pal.append(((c >> 7) & 0xf8, (c >> 2) & 0xf8, (c << 3) & 0xf8))
    return pal

def to_png(b, out):
    x, y, w, h = struct.unpack_from('<4H', b, 0)
    pal = palette(b)
    px = b[520:520 + w * h]
    raw = bytearray()
    for r in range(h):
        raw.append(0)
        for p in px[r * w:(r + 1) * w]:
            raw += bytes(pal[p])
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b'')
    open(out, 'wb').write(png)
    return x, y, w, h, len(b) - 520 - w * h

if __name__ == '__main__':
    b = lzh.decode(open(sys.argv[1], 'rb').read())
    print(sys.argv[1], to_png(b, sys.argv[2]))
