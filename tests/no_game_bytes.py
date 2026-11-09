#!/usr/bin/env python3
"""Check that built files hold no bytes taken from nanpa2.exe.

usage: no_game_bytes.py EXE PATH...

Every 64-byte run of the exe's sections (code, data, resources) that has at
least 12 different byte values is looked for in every PATH (files, or
folders such as the .app, scanned recursively). Runs with fewer values are
padding, zero fill and small-integer tables, which any program can share by
chance; so are runs of tables fixed by a public standard (standard_tables,
computed here) and linear ramps (ramps). A positive control first plants a run of the exe's code into
random bytes and must find it.

A second check looks for every 16-byte run of the .data with at least 8 different byte values
(the engine defines its start values in code; the only allowed .data text is the file names of ALLOWED).

Exit 0: nothing found; 1: a match (offsets are listed); 77: no exe to compare
with (skipped).
"""
import os
import random
import struct
import sys

WIN = 64
MIN_DISTINCT = 12


def sections(d):
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    n = struct.unpack_from('<H', d, pe + 6)[0]
    so = pe + 24 + struct.unpack_from('<H', d, pe + 20)[0]
    out = []
    for i in range(n):
        name = d[so + 40 * i:so + 40 * i + 8].rstrip(b'\0').decode('latin-1')
        rs, ro = struct.unpack_from('<II', d, so + 40 * i + 16)
        out.append((name, d[ro:ro + rs]))
    return out


def standard_tables():
    """64-byte runs of tables fixed by public standards, computed here, not
    taken from anywhere: the MP3 requantisation table x^(4/3) as floats
    (ISO 11172-3). The game's linked MP3 decoder and third_party/minimp3.h
    (g_pow43) both hold parts of it."""
    t = b''.join(struct.pack('<f', i ** (4 / 3)) for i in range(8207))
    return {t[o:o + WIN] for o in range(len(t) - WIN + 1)}


def ramps(b):
    """Byte ranges of b that are linear ramps: runs of 8-, 16- or 32-bit
    values whose steps differ by at most 1 (0, 4, 8, ... or i*1023/255
    rounded), at least WIN bytes long. Such tables are computed, not
    authored; SDL holds some that the game's data also has."""
    marked = bytearray(len(b))
    for e, fmt in ((1, 'B'), (2, 'H'), (4, 'I')):
        for phase in range(e):
            n = (len(b) - phase) // e
            v = struct.unpack_from('<%d%s' % (n, fmt), b, phase)
            start = 0
            lo = hi = None
            for j in range(1, n + 1):
                d = v[j] - v[j - 1] if j < n else None
                if d is not None and lo is not None and min(lo, d) + 1 >= max(hi, d):
                    lo, hi = min(lo, d), max(hi, d)
                    continue
                if (j - start) * e >= WIN and lo is not None:
                    a = phase + start * e
                    marked[a:phase + j * e] = b'\1' * (phase + j * e - a)
                start = j - 1
                lo = hi = d
    return marked


def index(secs):
    # 64-byte windows keyed by their first 8 bytes, then confirmed in full
    idx = {}
    total = 0
    std = {}
    known = standard_tables()
    for name, b in secs:
        ramp = ramps(b)
        for o in range(0, len(b) - WIN + 1):
            w = b[o:o + WIN]
            if len(set(w)) < MIN_DISTINCT:
                continue
            if w in known or all(ramp[o:o + WIN]):
                std[name] = std.get(name, 0) + 1
                continue
            total += 1
            idx.setdefault(w[:8], []).append((name, o))
    return idx, total, std


def scan(data, idx, secs_by_name, limit=10):
    hits = []
    for o in range(0, len(data) - WIN + 1):
        cand = idx.get(data[o:o + 8])
        if not cand:
            continue
        w = data[o:o + WIN]
        for name, so in cand:
            if secs_by_name[name][so:so + WIN] == w:
                hits.append((o, name, so))
                break
        if len(hits) >= limit:
            break
    return hits


SHORT = 16
SHORT_DISTINCT = 8
# the small format constants the source may hold (CLAUDE.md): file name formats of the game's own
# folders and files, which the engine needs to find them
ALLOWED = (b'SPEECH\\A\\A10930.WAV', b'MUSIC\\%02d.MP3', b'MUSIC\\%02d.WAV', b'%sSPEECH\\A\\A10930.WAV',
           b'start.mes\0\r\n$', b'flag0\0\r\n$')


def data_runs(secs):
    """Every 16-byte run of the exe's .data with at least 8 different byte values: tables, texts
    and code pointers authored by the game, short enough to be a string or a few words. A run
    made of allowed file-name constants and zero padding is not counted."""
    b = dict(secs)['.data']
    allowed = set()
    for a in ALLOWED:
        o = b.find(a)
        while o >= 0:
            allowed.update(range(o, o + len(a)))
            o = b.find(a, o + 1)
    runs = set()
    for o in range(len(b) - SHORT + 1):
        w = b[o:o + SHORT]
        if len(set(w)) >= SHORT_DISTINCT and not all(x in allowed or b[x] == 0 for x in range(o, o + SHORT)):
            runs.add(w)
    return runs


def scan_short(data, runs, limit=10):
    return [o for o in range(len(data) - SHORT + 1) if data[o:o + SHORT] in runs][:limit]


def files(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                for n in sorted(names):
                    f = os.path.join(root, n)
                    if os.path.isfile(f) and not os.path.islink(f):
                        yield f
        elif os.path.isfile(p):
            yield p
        else:
            print('not built, not checked: %s' % p)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    exe = sys.argv[1]
    if not os.path.isfile(exe):
        print('no %s to compare with: skipped' % exe)
        sys.exit(77)
    d = open(exe, 'rb').read()
    secs = sections(d)
    by_name = dict(secs)
    idx, total, std = index(secs)
    print('%s: %d runs of %d bytes in %s; in standard tables and ramps, not checked: %s'
          % (exe, total, WIN, ' '.join(n for n, _ in secs),
             ', '.join('%s %d' % kv for kv in sorted(std.items())) or 'none'))

    # positive control: a run of the code section planted in random bytes
    rnd = random.Random(1)
    text = by_name['.text']
    at = 0x2345
    while len(set(text[at:at + WIN])) < MIN_DISTINCT:
        at += WIN
    planted = bytes(rnd.randrange(256) for _ in range(5000)) + text[at:at + WIN] + \
        bytes(rnd.randrange(256) for _ in range(5000))
    if not scan(planted, idx, by_name):
        print('FAIL: the positive control (a planted run of .text) was not found')
        sys.exit(1)

    runs = data_runs(secs)
    print('%d distinct %d-byte runs of .data (8 or more different values) must not be in any file' %
          (len(runs), SHORT))
    planted_d = bytes(rnd.randrange(256) for _ in range(900)) + next(iter(runs)) + bytes(rnd.randrange(256) for _ in range(900))
    if not scan_short(planted_d, runs):
        print('FAIL: the positive control (a planted .data run) was not found')
        sys.exit(1)

    bad = 0
    n = 0
    for f in files(sys.argv[2:]):
        data = open(f, 'rb').read()
        n += 1
        short = scan_short(data, runs)
        if short:
            bad += 1
            print('FAIL: %s holds a %d-byte run of the .data of nanpa2.exe at file offsets %s' %
                  (f, SHORT, ', '.join('0x%x' % o for o in short)))
        hits = scan(data, idx, by_name)
        if hits:
            bad += 1
            print('FAIL: %s holds bytes of nanpa2.exe:' % f)
            for o, name, so in hits:
                print('  file offset 0x%x = %s + 0x%x' % (o, name, so))
    print('%d files checked, %d with bytes of nanpa2.exe' % (n, bad))
    sys.exit(1 if bad or not n else 0)


if __name__ == '__main__':
    main()
