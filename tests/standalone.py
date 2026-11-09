#!/usr/bin/env python3
"""The engine needs neither nanpa2.exe nor nanpa2.dat (R4: the start values of the game state are
defined in code, src/game/state.c): the game folder is the CD's DATA, MUSIC, SPEECH and the FLAG files.

usage: standalone.py EXE GAME_DIR SCRATCH_DIR
1. A folder of symlinks to GAME_DIR without nanpa2.exe and nanpa2.dat, and one with the whole of
   GAME_DIR, each run headless by the exploration bot for N frames with --record: every message
   and checkpoint (screen, palette, saved state, files opened, the G hash, the game clock) of the
   two recordings must be equal, and so must the FLAG files they wrote.
2. The recording of the folder with the files, replayed in the folder without them: every
   checkpoint matches (exit 0).
3. A folder holding a nanpa2.dat of any content (an old asset) and no exe runs the same: the file
   is not read.
Exit 0 ok, 1 failed, 77 skipped (no game folder)."""
import os
import shutil
import subprocess
import sys

exe, game, scratch = sys.argv[1:4]
if not os.path.isdir(os.path.join(game, "DATA")):
    print("no game folder: skipped")
    sys.exit(77)
shutil.rmtree(scratch, ignore_errors=True)
os.makedirs(scratch)
FRAMES = "2500"


def fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def folder(name, keep_all, dat=None):
    d = os.path.join(scratch, name)
    os.makedirs(d)
    for n in os.listdir(game):
        if n.lower() in ("nanpa2.dat", "nanpa2.exe") and not keep_all:
            continue
        os.symlink(os.path.abspath(os.path.join(game, n)), os.path.join(d, n))
    if dat is not None:
        with open(os.path.join(d, "nanpa2.dat"), "wb") as f:
            f.write(dat)
    return d


def run(name, gamedir, args, explore=True):
    save = os.path.join(scratch, name + "-save")
    os.makedirs(save, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith(("NANPA2_EXPLORE", "NANPA2_INPUT"))}
    env.update(NANPA2_HEADLESS="1", NANPA2_SAVE_DIR=save, NANPA2_SPEED="0", NANPA2_CP_TICKS="250")
    if explore:
        env.update(NANPA2_EXPLORE="31", NANPA2_EXPLORE_FRAMES=FRAMES)
    p = subprocess.run([exe, "--game", gamedir] + args, env=env, capture_output=True, text=True, timeout=300)
    return p, save


def lines(rec):
    return [l for l in open(rec).read().splitlines() if l.startswith(("cp ", "fin ", "ev "))]


def flags(save):
    return {n: open(os.path.join(save, n), "rb").read() for n in sorted(os.listdir(save))
            if n.upper().startswith("FLAG")}


full = folder("full", True)
bare = folder("bare", False)
recs = {}
saves = {}
for name, d in (("full", full), ("bare", bare)):
    rec = os.path.join(scratch, name + ".rec")
    p, saves[name] = run(name, d, ["--record", rec])
    if p.returncode != 0 or "game trap" in p.stderr or "WARNING" in p.stderr:
        fail("%s run: exit %d\n%s" % (name, p.returncode, p.stderr[-1500:]))
    recs[name] = lines(rec)
if len(recs["bare"]) < 10:
    fail("too little recorded: %d lines" % len(recs["bare"]))
if recs["bare"] != recs["full"]:
    for i, (x, y) in enumerate(zip(recs["bare"], recs["full"])):
        if x != y:
            fail("recordings differ at line %d: bare %s, full %s" % (i, x[:200], y[:200]))
    fail("recording lengths differ")
if flags(saves["bare"]) != flags(saves["full"]):
    fail("the FLAG files written differ")
print("no exe, no nanpa2.dat: %d recorded lines and %d FLAG files equal those with them" %
      (len(recs["bare"]), len(flags(saves["bare"]))))

# the recording replayed in the bare folder
p, _ = run("replay", bare, ["--replay", os.path.join(scratch, "full.rec")], explore=False)
if p.returncode != 0 or "replay: ok" not in p.stdout + p.stderr:
    fail("replay in the bare folder: exit %d\n%s" % (p.returncode, (p.stdout + p.stderr)[-1500:]))
print("replay in the bare folder: ok")

# an old asset (any bytes) is not read
junk = folder("junk", False, b"N2AS" + bytes(range(256)) * 4)
p, _ = run("junk", junk, [], explore=True)
if p.returncode != 0 or "game trap" in p.stderr:
    fail("a stray nanpa2.dat should be ignored:\n" + p.stderr[-800:])
print("a stray nanpa2.dat: ignored")
