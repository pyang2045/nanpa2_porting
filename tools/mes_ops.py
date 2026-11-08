#!/usr/bin/env python3
"""Which extended opcodes (04 xx) do the game's MES scripts use?

Reads the player's own DATA folder (nothing is copied out of it) and counts
the byte pairs 04 xx, xx = 0x10-0x26. A pair inside a string or an argument
can be a false hit, so a count is an upper bound; a zero is a proof of
absence. Stage 6 used it to find that no shipped script uses ext 20 (set a
click handler, 0x4070bc) or ext 1a, so the VM's click handler path
(0x40613f-0x40616c) is reached only through a modified script.

usage: mes_ops.py GAME_DIR"""
import collections
import glob
import os
import sys

root = os.path.join(sys.argv[1], "DATA")
files = [f for f in glob.glob(os.path.join(root, "*")) if f.upper().endswith(".MES")]
count = collections.Counter()
where = collections.defaultdict(list)
for f in sorted(files):
    d = open(f, "rb").read()
    for i in range(len(d) - 1):
        if d[i] == 4 and 0x10 <= d[i + 1] <= 0x26:
            count[d[i + 1]] += 1
            if len(where[d[i + 1]]) < 3:
                where[d[i + 1]].append(os.path.basename(f))
print(len(files), "MES files")
for op in range(0x10, 0x27):
    print("04 %02x: %6d %s" % (op, count[op], " ".join(where[op])))
