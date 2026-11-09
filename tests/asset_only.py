#!/usr/bin/env python3
"""nanpa2.dat (src/asset.c, docs/reimpl.md "The asset"): the engine runs
from the asset alone.

1. `nanpa2 --make-assets` cuts the asset from the exe; it must be a valid
   container whose ranges avoid the game code 0x401000-0x4077ff, and none of
   its bytes may be a 64-byte run of that code (or of the rest of .text);
2. a game folder without nanpa2.exe (symlinks to the real one) and with the
   asset in it runs headless to a scripted point on game time
   (NANPA2_INPUT_CLOCK=frames) with no trap, and its screenshots and its recording (every
   checkpoint: screen, palette, saved state, files opened, the G hash, the game clock) equal
   those of the same run with the whole exe's data loaded (NANPA2_LOAD_EXE=1);
3. a folder with the exe and no asset makes one at the first launch;
4. a folder with neither stops with a clear message.

usage: asset_only.py EXE GAME_DIR SCRATCH_DIR
Exit 0 ok, 1 failed, 77 skipped (no nanpa2.exe to cut the asset from)."""
import hashlib
import os
import shutil
import struct
import subprocess
import sys

exe, game, scratch = sys.argv[1:4]
src_exe = os.path.join(game, "nanpa2.exe")
if not os.path.exists(src_exe):
    print("no nanpa2.exe in", game, "- skipped")
    sys.exit(77)
shutil.rmtree(scratch, ignore_errors=True)
os.makedirs(scratch)


def fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def folder(name, with_exe):
    d = os.path.join(scratch, name)
    os.makedirs(d)
    for n in os.listdir(game):
        if n.lower() == "nanpa2.dat" or (n.lower() == "nanpa2.exe" and not with_exe):
            continue  # the game folder may hold an asset of an earlier launch
        os.symlink(os.path.abspath(os.path.join(game, n)), os.path.join(d, n))
    return d


def run(name, gamedir, extra=None, quit_ms=None, timeout=300, shots=False):
    save = os.path.join(scratch, name + "-save")
    os.makedirs(save, exist_ok=True)
    env = dict(os.environ, NANPA2_HEADLESS="1", NANPA2_SAVE_DIR=save, NANPA2_INPUT_CLOCK="frames",
               NANPA2_CP_TICKS="100", **(extra or {}))
    if quit_ms:
        env["NANPA2_INPUT"] = "quit,%d" % quit_ms
    if shots:
        env["NANPA2_SHOT_DIR"] = os.path.join(scratch, name + "-shots")
        os.makedirs(env["NANPA2_SHOT_DIR"], exist_ok=True)
        env["NANPA2_SHOT_SCHEDULE"] = "4000,4000,5"
    args = [exe, "--game", gamedir]
    if shots:
        args += ["--record", os.path.join(scratch, name + ".rec")]
    p = subprocess.run(args, env=env, capture_output=True, text=True, timeout=timeout)
    return p


# ---- 1. the asset
asset = os.path.join(scratch, "nanpa2.dat")
p = subprocess.run([exe, "--make-assets", "--game", game, "--out", asset], capture_output=True, text=True,
                   timeout=120)
if p.returncode != 0 or not os.path.exists(asset):
    fail("--make-assets: " + p.stdout + p.stderr)
a = open(asset, "rb").read()
magic, ver, stamp, isize, esize, n, ehash, chash, payload, _ = struct.unpack_from("<4sIIIIIQQII", a, 0)
if magic != b"N2AS" or ver != 1 or stamp != 0x39697C6A or isize != 0xC1000:
    fail("bad header")
tab = struct.unpack_from("<%dI" % (3 * n), a, 48)
rng = [(tab[3 * i], tab[3 * i + 1], tab[3 * i + 2]) for i in range(n)]
if 48 + 12 * n + payload != len(a):
    fail("size does not match the header")
for va, sz, kind in rng:
    if va < 0x400000 or va + sz > 0x400000 + isize:
        fail("range outside the image: %x+%x" % (va, sz))
    if va < 0x407800 and va + sz > 0x401000:
        fail("range %x+%x overlaps the game code" % (va, sz))
e = open(src_exe, "rb").read()
text = e[0x1000:0x1000 + 0x1A000]  # .text: the game code, the MP3 library, the C runtime
runs = {text[o:o + 64] for o in range(0, len(text) - 63, 16) if len(set(text[o:o + 64])) >= 12}
bad = [o for o in range(len(a) - 63) if a[o:o + 64] in runs]
if bad:
    fail("the asset holds 64-byte runs of .text at file offsets %s" % bad[:5])
print("asset: %d bytes, %d ranges (%d stored, %d zero), no code" %
      (len(a), n, sum(1 for r in rng if r[2] == 0), sum(1 for r in rng if r[2] == 1)))

# ---- 2. exe hidden, asset in the folder, against the whole exe
hidden = folder("hidden", False)
shutil.copy(asset, os.path.join(hidden, "nanpa2.dat"))
whole = folder("whole", True)
trace = {}
digests = {}
for name, d, extra in (("asset", hidden, {}), ("exe", whole, {"NANPA2_LOAD_EXE": "1"})):
    p = run(name, d, extra, quit_ms=24000, shots=True)
    if p.returncode != 0 or "game trap" in p.stderr or "WARNING" in p.stderr:
        fail("%s run: exit %d\n%s" % (name, p.returncode, p.stderr[-1500:]))
    want = "assets: " + os.path.join(d, "nanpa2.dat") if name == "asset" else "loaded whole"
    if want not in p.stderr:
        fail("%s run did not log %r" % (name, want))
    trace[name] = [ln for ln in open(os.path.join(scratch, name + ".rec")).read().splitlines()
                   if ln.startswith(("cp ", "fin ", "ev "))]
    shots = sorted(os.listdir(os.path.join(scratch, name + "-shots")))
    digests[name] = [hashlib.md5(open(os.path.join(scratch, name + "-shots", s), "rb").read()).hexdigest()
                     for s in shots]
if len(trace["asset"]) < 6 or len(digests["asset"]) < 3:
    fail("too little recorded: %d lines, %d screenshots" % (len(trace["asset"]), len(digests["asset"])))
if trace["asset"] != trace["exe"]:
    for i, (x, y) in enumerate(zip(trace["asset"], trace["exe"])):
        if x != y:
            fail("recordings differ at line %d: asset %s, exe %s" % (i, x[:200], y[:200]))
    fail("recording lengths differ: %d, %d" % (len(trace["asset"]), len(trace["exe"])))
if digests["asset"] != digests["exe"]:
    fail("screenshots differ: %s vs %s" % (digests["asset"], digests["exe"]))
print("asset-only run: %d recorded lines and %d screenshots equal those of the whole-exe run" %
      (len(trace["asset"]), len(digests["asset"])))

# ---- 3. first launch with the exe makes the asset
first = folder("first", True)
p = run("first", first, quit_ms=3000)
made = os.path.join(first, "nanpa2.dat")
if p.returncode != 0 or not os.path.isfile(made) or os.path.islink(made):
    fail("first launch did not make nanpa2.dat:\n" + p.stderr[-1500:])
if open(made, "rb").read() != a:
    fail("the asset made at launch differs from --make-assets")
print("first launch made nanpa2.dat")

# ---- 4. neither
none = folder("none", False)
p = run("none", none, quit_ms=3000)
if p.returncode == 0 or "no nanpa2.dat and no nanpa2.exe" not in p.stderr:
    fail("a folder with neither should say so:\n" + p.stderr[-800:])
print("neither: refused with a message")

# ---- 5. a damaged asset with no exe
bad_dir = folder("damaged", False)
dam = bytearray(a)
dam[200] ^= 0xFF
open(os.path.join(bad_dir, "nanpa2.dat"), "wb").write(dam)
p = run("damaged", bad_dir, quit_ms=3000)
if p.returncode == 0 or "damaged" not in p.stderr:
    fail("a damaged asset should be refused:\n" + p.stderr[-800:])
print("damaged asset: refused")
