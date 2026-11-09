#!/usr/bin/env python3
"""Deterministic mode, recording and replay (src/hle/replay.h), checked on the
player's own game data; nothing derived from it is kept in the repository.

  tests/replay_det.py BUILD/nanpa2 GAME_DIR WORK_DIR [--frames N]

1. A new game played by the exploration bot for N frames, recorded (--record).
2. The same again from a fresh save folder: the two recordings (messages and
   every checkpoint, the raw memory hashes included) must be identical, and so
   must the FLAG files written.
3. Replay of the first recording: every checkpoint matches, exit 0.
4. A recording with one checkpoint hash changed: the replay stops with exit 3
   and names the checkpoint and the key that differs.
5. The recording with a click left out: the replay diverges.

Exit codes: 0 pass, 77 skip (no game folder), 1 failure.
"""
import os
import shutil
import subprocess
import sys

SKIP = 77


def run(exe, game, work, name, args, env_extra, timeout=300):
    d = os.path.join(work, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "saves"))
    env = dict(os.environ, NANPA2_HEADLESS="1", NANPA2_SAVE_DIR=os.path.join(d, "saves"),
               NANPA2_CPU_MIPS="0", NANPA2_CP_TICKS="250")
    for k in [k for k in env if k.startswith(("NANPA2_EXPLORE", "NANPA2_INPUT", "NANPA2_REPLAY", "NANPA2_RECORD"))]:
        del env[k]
    env.update(env_extra)
    p = subprocess.run([exe, "--game", game] + args, env=env, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr, d


def flags(d):
    out = {}
    s = os.path.join(d, "saves")
    for n in sorted(os.listdir(s)):
        with open(os.path.join(s, n), "rb") as f:
            out[n] = f.read()
    return out


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 1
    exe, game, work = sys.argv[1:4]
    frames = sys.argv[sys.argv.index("--frames") + 1] if "--frames" in sys.argv else "2500"
    if not os.path.isdir(os.path.join(game, "DATA")):
        print("no game folder: skipped")
        return SKIP
    os.makedirs(work, exist_ok=True)
    here = os.path.dirname(os.path.abspath(__file__))
    diff = os.path.join(here, "..", "tools", "replay_diff.py")
    explore = {"NANPA2_EXPLORE": "31", "NANPA2_EXPLORE_FRAMES": frames}
    ra, rb = os.path.join(work, "a.rec"), os.path.join(work, "b.rec")
    rc, out, da = run(exe, game, work, "a", ["--record", ra], explore)
    if rc:
        print(out[-2000:])
        print("FAIL: recording run exited %d" % rc)
        return 1
    rc, out, db = run(exe, game, work, "b", ["--record", rb], explore)
    if rc:
        print(out[-2000:])
        print("FAIL: second recording run exited %d" % rc)
        return 1
    n_cp = sum(1 for l in open(ra) if l.startswith("cp "))
    n_ev = sum(1 for l in open(ra) if l.startswith("ev "))
    if n_cp < 3 or n_ev < 20:
        print("FAIL: the recording is too short (%d messages, %d checkpoints)" % (n_ev, n_cp))
        return 1
    p = subprocess.run([sys.executable, diff, ra, rb], capture_output=True, text=True)
    print(p.stdout.strip())
    if p.returncode:
        print("FAIL: two recordings of the same input differ")
        return 1
    if flags(da) != flags(db):
        print("FAIL: the FLAG files differ between the runs")
        return 1
    rc, out, _ = run(exe, game, work, "r1", ["--replay", ra], {})
    if rc or "replay: ok" not in out:
        print(out[-2000:])
        print("FAIL: the replay did not match (exit %d)" % rc)
        return 1
    print([l for l in out.splitlines() if l.startswith("replay: ok")][0])
    # a changed checkpoint hash is found, and named
    lines = open(ra).read().split("\n")
    cps = [i for i, l in enumerate(lines) if l.startswith("cp ")]
    i = cps[len(cps) // 2]
    lines[i] = lines[i].replace("li.screen=", "li.screen=f", 1)
    bad = os.path.join(work, "bad.rec")
    open(bad, "w").write("\n".join(lines))
    rc, out, _ = run(exe, game, work, "r2", ["--replay", bad], {})
    if rc != 3 or "DIVERGED" not in out or "li.screen" not in out:
        print(out[-1500:])
        print("FAIL: a changed checkpoint was not reported (exit %d)" % rc)
        return 1
    print([l for l in out.splitlines() if "DIVERGED" in l][0])
    # a message left out changes the play
    lines = open(ra).read().split("\n")
    clicks = [i for i, l in enumerate(lines) if l.startswith("ev ") and l.split()[3] == "201"]
    del lines[clicks[len(clicks) // 3]]
    cut = os.path.join(work, "cut.rec")
    open(cut, "w").write("\n".join(lines))
    rc, out, _ = run(exe, game, work, "r3", ["--replay", cut], {})
    if rc != 3 or "DIVERGED" not in out:
        print(out[-1500:])
        print("FAIL: a recording with a click left out did not diverge (exit %d)" % rc)
        return 1
    print([l for l in out.splitlines() if "DIVERGED" in l][0][:160])
    print("replay_det: ok (%d messages, %d checkpoints)" % (n_ev, n_cp))
    return 0


sys.exit(main())
