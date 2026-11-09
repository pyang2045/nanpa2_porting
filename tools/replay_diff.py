#!/usr/bin/env python3
"""Compare two recordings (nanpa2 --record, src/dev/replay.h) and report the first
divergent checkpoint and which state differs.

    tools/replay_diff.py A.rec B.rec [--li]
    tools/replay_diff.py --sequence A.rec B.rec [--logs A.log B.log]
    tools/replay_diff.py --sequence --logs A.log B.log

--li: only the layout independent keys (li.*). Keys that one recording has and the other does
not are skipped when they are the retired ones of the emulated machine (raw.cpu, raw.data, raw.heap,
raw.stack, raw.pg: recordings up to R3), so a recording of R3 and one of R4 compare on li.*,
raw.clock and (if both have it) raw.gdata. Exit 0 when the checkpoints (and messages) agree, 1
otherwise.

--sequence: compare two recordings made with different pacing (another clock, so other
ticks and other message times) by the content of the states they went through, not by
when: the cumulative MES-file chain (`li.mes`: for every count both recordings reached,
the hash must be equal, i.e. the same files in the same order), the FLAG files written
(`li.flags`: the distinct states in order, and the last), the quiescent screens (a
`li.screen`/`li.pal` pair that stands over two or more checkpoints) as a longest common
subsequence, and the last state. --logs adds the names: NANPA2_TIMELOG output of a replay of
each (the `DATA\\NAME.MES` lines), compared as ordered lists. Exit 0 when the MES chain, the
FLAG states and the logs agree; the screen comparison is reported only (checkpoints fall
at other points of an animation).
"""
import sys

RETIRED = ("raw.cpu", "raw.data", "raw.heap", "raw.stack", "raw.pg")


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


def lcs(a, b):
    """length of the longest common subsequence, and the elements of a not in it"""
    n, m = len(a), len(b)
    t = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(n - 1, -1, -1):
        for j in range(m - 1, -1, -1):
            t[i][j] = t[i + 1][j + 1] + 1 if a[i] == b[j] else max(t[i + 1][j], t[i][j + 1])
    i = j = 0
    miss = []
    while i < n and j < m:
        if a[i] == b[j]:
            i += 1
            j += 1
        elif t[i + 1][j] >= t[i][j + 1]:
            miss.append(i)
            i += 1
        else:
            j += 1
    miss += list(range(i, n))
    return t[0][0], miss


def collapse(seq):
    out = []
    for x in seq:
        if not out or out[-1] != x:
            out.append(x)
    return out


def quiescent(cps):
    """(screen, pal) pairs that stand over two or more consecutive checkpoints, in order"""
    out, run, prev = [], 0, None
    for _, _, _, kv in cps:
        cur = (kv.get("li.screen"), kv.get("li.pal"))
        if cur == prev:
            run += 1
            if run == 2:
                out.append(cur)
        else:
            run, prev = 1, cur
    return collapse(out)


def log_files(path):
    names = []
    with open(path, errors="replace") as f:
        for line in f:
            a = line.split()
            if len(a) >= 4 and a[0] == "T" and a[3].upper().endswith(".MES"):
                names.append(a[3].upper())
    return names


def sequence(argv):
    rest = [a for a in argv if a != "--sequence"]
    logs = []
    if "--logs" in rest:
        i = rest.index("--logs")
        logs = rest[i + 1:i + 3]
        rest = rest[:i] + rest[i + 3:]
    if (len(rest) != 2 and not (logs and not rest)) or (logs and len(logs) != 2):
        print(__doc__)
        return 2
    rc = 0
    ca = cb = []
    if rest:
        _, _, ca = parse(rest[0])
        _, _, cb = parse(rest[1])

    def chain(cps):
        d = {}
        for _, _, _, kv in cps:
            m = kv.get("li.mes")
            if m:
                n, h = m.split("/")
                d[int(n)] = h
        return d

    if rest:
        ma, mb = chain(ca), chain(cb)
        common = sorted(set(ma) & set(mb))
        bad = [n for n in common if ma[n] != mb[n]]
        print("MES chain: A reaches %d files, B %d; %d counts in common, %d with a different chain hash"
              % (max(ma, default=0), max(mb, default=0), len(common), len(bad)))
        if bad:
            print("  first different count: %d" % bad[0])
            rc = 1
        last = min(max(ma, default=0), max(mb, default=0))
        if last in ma and last in mb and ma[last] == mb[last]:
            print("  the first %d files are the same, in the same order" % last)

        fa = collapse([kv.get("li.flags") for _, _, _, kv in ca])
        fb = collapse([kv.get("li.flags") for _, _, _, kv in cb])
        n, miss = lcs(fa, fb)
        print("FLAG states (distinct, in order): A %d, B %d, common subsequence %d; last equal: %s"
              % (len(fa), len(fb), n, fa[-1:] == fb[-1:]))
        if n != len(fa) or n != len(fb):
            rc = 1

        qa, qb = quiescent(ca), quiescent(cb)
        n, miss = lcs(qa, qb)
        print("quiescent screens (screen+palette standing over 2+ checkpoints): A %d, B %d, common "
              "subsequence %d (%.0f%% of A, %.0f%% of B)"
              % (len(qa), len(qb), n, 100.0 * n / max(1, len(qa)), 100.0 * n / max(1, len(qb))))
        sa = [(x[3].get("li.screen"), x[3].get("li.pal")) for x in ca[-1:]]
        sb = [(x[3].get("li.screen"), x[3].get("li.pal")) for x in cb[-1:]]
        print("last checkpoint screen+palette equal: %s" % (sa == sb))
    if logs:
        la, lb = log_files(logs[0]), log_files(logs[1])
        k = min(len(la), len(lb))
        same = la[:k] == lb[:k]
        print("MES files opened (names, from the logs): A %d, B %d; the first %d %s"
              % (len(la), len(lb), k, "are identical" if same else "DIFFER"))
        if not same:
            d = next(i for i in range(k) if la[i] != lb[i])
            print("  first difference at %d: %s against %s (context %s | %s)"
                  % (d, la[d], lb[d], la[max(0, d - 2):d + 2], lb[max(0, d - 2):d + 2]))
            rc = 1
    return rc


def main():
    if "--sequence" in sys.argv:
        return sequence(sys.argv[1:])
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
        keys = [k for k in x[3] if (not li or k.startswith("li.")) and not (k in RETIRED and k not in y[3])]
        bad = [k for k in keys if x[3][k] != y[3].get(k)]
        if x[:3] != y[:3]:
            print("checkpoint %d taken at tick %d pump %d against tick %d pump %d" % (i, x[1], x[2], y[1], y[2]))
            rc = 1
        if bad:
            print("first divergent checkpoint: %d (%s tick %d pump %d)" % (i, x[0], x[1], x[2]))
            for k in bad:
                print("  %s: %s  %s" % (k, x[3][k], y[3].get(k)))
            return 1
    if len(ca) != len(cb):
        print("%d checkpoints against %d" % (len(ca), len(cb)))
        rc = 1
    if rc == 0:
        print("identical: %d messages, %d checkpoints" % (len(ea), len(ca)))
    return rc


sys.exit(main())
