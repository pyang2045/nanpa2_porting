#!/usr/bin/env python3
"""Compare two recordings (nanpa2 --record, src/hle/replay.h) and report the first
divergent checkpoint and which state differs.

    tools/replay_diff.py A.rec B.rec [--li]

--li: only the layout independent keys (li.*), for runs of code with a different
memory layout. Exit 0 when the checkpoints (and messages) agree, 1 otherwise.
"""
import sys

PAGE_LO, PAGE = 0x431000, 4096


def parse(path):
    head, events, cps = [], [], []
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("ev "):
                events.append(line)
            elif line.startswith(("cp ", "fin ")):
                kind, tick, k, rest = line.split(" ", 3)
                kv = dict(t.split("=", 1) for t in rest.split(" ") if "=" in t)
                cps.append((kind, int(tick), int(k), kv))
            else:
                head.append(line)
    return head, events, cps


def pages(a, b):
    return ["%06x" % (PAGE_LO + i * PAGE) for i in range(min(len(a), len(b)) // 4)
            if a[i * 4:i * 4 + 4] != b[i * 4:i * 4 + 4]]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    li = "--li" in sys.argv
    if len(args) != 2:
        print(__doc__)
        return 2
    ha, ea, ca = parse(args[0])
    hb, eb, cb = parse(args[1])
    rc = 0
    if ha != hb:
        print("headers differ:", ha, hb)
        rc = 1
    for i, (x, y) in enumerate(zip(ea, eb)):
        if x != y:
            print("message %d differs:\n  %s\n  %s" % (i, x, y))
            rc = 1
            break
    if len(ea) != len(eb):
        print("%d messages against %d" % (len(ea), len(eb)))
        rc = 1
    for i, (x, y) in enumerate(zip(ca, cb)):
        keys = [k for k in x[3] if not li or k.startswith("li.")]
        bad = [k for k in keys if x[3][k] != y[3].get(k)]
        if x[:3] != y[:3]:
            print("checkpoint %d taken at tick %d pump %d against tick %d pump %d" % (i, x[1], x[2], y[1], y[2]))
            rc = 1
        if bad:
            print("first divergent checkpoint: %d (%s tick %d pump %d)" % (i, x[0], x[1], x[2]))
            for k in bad:
                if k == "raw.pg":
                    print("  raw.pg: pages differ at", " ".join(pages(x[3][k], y[3].get(k, ""))[:16]))
                else:
                    print("  %s: %s  %s" % (k, x[3][k], y[3].get(k)))
            return 1
    if len(ca) != len(cb):
        print("%d checkpoints against %d" % (len(ca), len(cb)))
        rc = 1
    if rc == 0:
        print("identical: %d messages, %d checkpoints" % (len(ea), len(ca)))
    return rc


sys.exit(main())
