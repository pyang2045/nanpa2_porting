#!/usr/bin/env python3
"""Summarise the user-visible timings in NANPA2_TIMELOG output.

  tools/timing_stats.py LOG [LOG...]

A log line is `T <game ms> <frame> <label> <arg>` (src/game/ctl.c ctl_tlog). Several logs are pooled.
Reports, in game milliseconds (= real milliseconds at speed 1.0):
  fade      fade+ to fade-
  wipe      wipe+ to wipe-, in the long (waits taken) and short (a button held) groups
  text      bursts of `ch` lines (gap < 400 ms): chars, ms, ms per char
  walk      runs of `step` lines (gap < 150 ms): steps, ms, ms per step
"""
import statistics as st
import sys


def load(paths):
    out = []
    for p in paths:
        ev = []
        with open(p, errors="replace") as f:
            for line in f:
                if line.startswith("T "):
                    a = line.split(None, 4)
                    if len(a) >= 4:
                        ev.append((int(a[1]), a[3]))
        out.append(ev)
    return out


def pairs(logs, a, b):
    d = []
    for ev in logs:
        t0 = None
        for t, lab in ev:
            if lab == a:
                t0 = t
            elif lab == b and t0 is not None:
                d.append(t - t0)
                t0 = None
    return d


def runs(logs, lab, gap):
    r = []
    for ev in logs:
        cur = None
        for t, l in ev:
            if l != lab:
                continue
            if cur and t - cur[1] <= gap:
                cur[1] = t
                cur[2] += 1
            else:
                if cur:
                    r.append(cur)
                cur = [t, t, 1]
        if cur:
            r.append(cur)
    return r


def summ(name, v, unit="ms"):
    if not v:
        print("%-28s none" % name)
        return
    v = sorted(v)
    print("%-28s n=%-6d min %-8.1f median %-8.1f mean %-8.1f max %-8.1f %s" %
          (name, len(v), v[0], st.median(v), st.mean(v), v[-1], unit))


def main():
    logs = load(sys.argv[1:])
    summ("fade", pairs(logs, "fade+", "fade-"))
    w = pairs(logs, "wipe+", "wipe-")
    summ("wipe, all", w)
    summ("wipe, long (>2500 ms)", [x for x in w if x > 2500])
    summ("wipe, short (<=2500 ms)", [x for x in w if x <= 2500])
    t = runs(logs, "ch", 400)
    summ("text burst ms", [b[1] - b[0] for b in t if b[2] >= 20])
    summ("text ms per char (>=20 ch)", [(b[1] - b[0]) / (b[2] - 1) for b in t if b[2] >= 20])
    big = [b for b in t if b[2] >= 40]
    summ("text burst of >=40 chars, ms", [b[1] - b[0] for b in big])
    wk = runs(logs, "step", 150)
    summ("walk step interval, ms", [(b[1] - b[0]) / (b[2] - 1) for b in wk if b[2] >= 10])
    summ("walk run length, steps", [b[2] for b in wk if b[2] >= 10], "steps")
    summ("walk run time, ms", [b[1] - b[0] for b in wk if b[2] >= 10])


main()
