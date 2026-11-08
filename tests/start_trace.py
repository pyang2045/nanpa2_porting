#!/usr/bin/env python3
"""Start-up check (docs/reimpl.md, Stage 6): WinMain (0x402090), the CD search,
the display set-up and the shutdown are C and cannot be compared online (they
create the window, DirectDraw and the timer, which cannot be done twice).
NANPA2_START_ORACLE=1 runs the original WinMain and shutdown instead, so this
test runs the game headless twice, with the C start-up and with the original,
and compares every call into the HLE layer (NANPA2_HLETRACE: imports, COM
methods, C runtime) from launch to a quit a few seconds in.

Not compared: the message pump's calls (their number depends on timing and
their argument is junk) and the argument slot of the sound exit 0x402c70,
which takes none (the original leaves a stale word there).

usage: start_trace.py EXE GAME_DIR SCRATCH_DIR
Exit 0 equal, 1 different, 77 skipped (no game folder)."""
import os
import subprocess
import sys

exe, game, scratch = sys.argv[1:4]
if not os.path.exists(os.path.join(game, "nanpa2.exe")):
    print("no nanpa2.exe in", game, "- skipped")
    sys.exit(77)


def trace(name, extra):
    save = os.path.join(scratch, name)
    os.makedirs(save, exist_ok=True)
    env = dict(os.environ, NANPA2_HEADLESS="1", NANPA2_HLETRACE="1", NANPA2_INPUT="quit,6000",
               NANPA2_SAVE_DIR=save, **extra)
    p = subprocess.run([exe, "--game", game], env=env, capture_output=True, text=True, timeout=120)
    out = []
    for ln in p.stderr.splitlines():
        if not ln.startswith("hle "):
            continue
        if ln.startswith("hle 00401bf0"):
            continue
        if ln.startswith("hle 00402c70"):
            ln = "hle 00402c70"
        out.append(ln)
    if p.returncode != 0 or "x86 trap" in p.stderr:
        print(name, "exit", p.returncode)
        print(p.stderr[-2000:])
        sys.exit(1)
    return out


a = trace("c-start", {})
b = trace("original-start", {"NANPA2_START_ORACLE": "1"})
if len(a) < 100:
    print("too few HLE calls traced:", len(a))
    sys.exit(1)
if a != b:
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            print("first difference at call", i, "C:", x, "original:", y)
            break
    else:
        print("lengths differ", len(a), len(b))
    sys.exit(1)
print("start-up: %d HLE calls, the C start-up and the original agree" % len(a))
