#!/usr/bin/env python3
"""Read bytes/strings from nanpa2.exe by virtual address.
usage: peek.py EXE ADDR [LEN] [s|x|w]   (s=C string, x=hexdump, w=u16 list)"""
import struct, sys

def load(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    n = struct.unpack_from('<H', d, pe + 6)[0]
    base = struct.unpack_from('<I', d, pe + 52)[0]
    secs = pe + 24 + struct.unpack_from('<H', d, pe + 20)[0]
    S = []
    for i in range(n):
        o = secs + i * 40
        vs, va, rs, ro = struct.unpack_from('<IIII', d, o + 8)
        S.append((base + va, vs, rs, ro))
    return d, S

def read(d, S, va, ln):
    out = b''
    for a in range(va, va + ln):
        for sva, vs, rs, ro in S:
            if sva <= a < sva + max(vs, rs):
                out += bytes([d[ro + a - sva]]) if a - sva < rs else b'\0'
                break
        else:
            out += b'?'
    return out

if __name__ == '__main__':
    d, S = load(sys.argv[1])
    va = int(sys.argv[2], 16)
    ln = int(sys.argv[3], 0) if len(sys.argv) > 3 else 64
    mode = sys.argv[4] if len(sys.argv) > 4 else 's'
    b = read(d, S, va, ln)
    if mode == 's':
        print(repr(b.split(b'\0')[0].decode('big5', 'replace')))
    elif mode == 'w':
        print(' '.join('%04x' % x for x in struct.unpack('<%dH' % (ln // 2), b)))
    else:
        for i in range(0, ln, 16):
            print('%08x: %s' % (va + i, b[i:i + 16].hex(' ')))
