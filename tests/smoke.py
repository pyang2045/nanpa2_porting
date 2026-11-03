#!/usr/bin/env python3
"""Smoke test of a plain interactive launch: a real window and presenter, no
scripted-mode environment. Reads the live status file the game writes
($TMPDIR/nanpa2-status-<pid>.json, src/hle/status.c) once a second.

  tests/smoke.py BUILD/nanpa2 GAME_DIR [--seconds 35] [--verbose]

Checks, for the interactive run:
  - exactly one window (SDL and on screen), shown
  - frames reach the window, and a non-black one by T=20 s
  - the game's frame counter (0x446292) advances
  - no watchdog warnings
then, for a NANPA2_HEADLESS=1 run (as the scripted test runs use): no window
on screen. Exits 0 on success, 1 with the diagnosis (the last status) on
failure. Uses only the Python standard library.
"""
import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import time


def status_path(pid):
    return os.path.join(os.environ.get("TMPDIR") or "/tmp", "nanpa2-status-%d.json" % pid)


def clean_env(extra=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("NANPA2_")}
    env.update(extra or {})
    return env


def run(binary, game, seconds, env, verbose, label):
    """Launch, sample the status file each second; returns (samples, log)."""
    log = tempfile.TemporaryFile(mode="w+")
    p = subprocess.Popen([binary, "--game", game], env=env, stdout=log, stderr=subprocess.STDOUT)
    path = status_path(p.pid)
    samples = []
    t0 = time.time()
    try:
        while time.time() - t0 < seconds:
            time.sleep(1)
            if p.poll() is not None:
                break
            try:
                with open(path) as f:
                    d = json.load(f)
            except (OSError, ValueError):
                continue
            samples.append(d)
            if verbose:
                pr, g, w = d["present"], d["game"], d["window"]
                print("%s %5.1fs win %d/%d occl %d focus %d in %d shown %d skip %s wait %.0f "
                      "luma %d/%d black %d frame %d active %d pc %s logo %d music %d %s" % (
                          label, d["uptime_ms"] / 1000, w["count"], w["os_windows"], w["occluded"],
                          w["focus"], pr["frames_in"], pr["presented"],
                          "/".join(str(v) for v in pr["skipped"].values()), pr["worst_wait_ms"],
                          pr["luma_in"], pr["luma_shown"], pr["black_ms"], g["frame"],
                          g["active_4ac4ec"], g["poll_pc"], d["logo"], d["music"], d["warning"]),
                      flush=True)
    finally:
        exited = p.poll()
        if exited is None:
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        log.seek(0)
        text = log.read()
    if exited is not None:
        samples.append({"exited": exited})
    if os.path.exists(path):
        samples.append({"stale_status_file": path})
    return samples, text


def check_interactive(samples):
    errs = []
    st = [s for s in samples if "pid" in s]
    if not st:
        return ["no status file was written"]
    for s in samples:
        if "exited" in s:
            errs.append("the game exited early (code %s)" % s["exited"])
        if "stale_status_file" in s:
            errs.append("status file left behind: %s" % s["stale_status_file"])
    last = st[-1]
    for s in st:
        w = s["window"]
        if w["count"] != 1 or w["os_windows"] not in (1, -1) or not w["shown"]:
            errs.append("at %.1f s: %d SDL windows, %d on screen, shown %d (want exactly one)" % (
                s["uptime_ms"] / 1000, w["count"], w["os_windows"], w["shown"]))
            break
    if last["present"]["presented"] == 0:
        errs.append("no frame reached the window")
    if not any(s["uptime_ms"] <= 20000 and s["present"]["luma_shown"] >= 4 for s in st):
        errs.append("no non-black frame presented by T=20 s")
    frames = [s["game"]["frame"] for s in st]
    if len(frames) < 2 or frames[-1] <= frames[0] or last["game"]["frame_still_ms"] > 3000:
        errs.append("the frame counter does not advance (%s)" % frames[-5:])
    if last["warnings"]:
        warned = [s["warning"] for s in st if s["warning"]]
        errs.append("%d watchdog warning(s): %s" % (last["warnings"], warned[0] if warned else "?"))
    return errs


def check_headless(samples):
    st = [s for s in samples if "pid" in s]
    if not st:
        return ["headless: no status file was written"]
    errs = []
    for s in st:
        w = s["window"]
        if w["shown"] or w["os_windows"] > 0:
            errs.append("headless: a window is on screen (flags %#x, os_windows %d)" % (
                w["flags"], w["os_windows"]))
            break
    if not st[-1]["mode"]["headless"]:
        errs.append("headless: the status file does not record headless mode")
    return errs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("game")
    ap.add_argument("--seconds", type=float, default=35)
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    samples, log = run(a.binary, a.game, a.seconds, clean_env(), a.verbose, "interactive")
    errs = check_interactive(samples)
    hs, hlog = run(a.binary, a.game, 6, clean_env({"NANPA2_HEADLESS": "1"}), a.verbose, "headless")
    errs += check_headless(hs)
    if errs:
        print("SMOKE FAIL")
        for e in errs:
            print("  " + e)
        st = [s for s in samples if "pid" in s]
        if st:
            print("last status:\n" + json.dumps(st[-1], indent=2))
        print("log (interactive):\n" + "\n".join(log.splitlines()[-30:]))
        return 1
    last = [s for s in samples if "pid" in s][-1]
    print("SMOKE OK: one window, %d frames presented, frame counter %d, luma %d, no warnings" % (
        last["present"]["presented"], last["game"]["frame"], last["present"]["luma_shown"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
