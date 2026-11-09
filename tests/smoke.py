#!/usr/bin/env python3
"""Smoke test of a plain interactive launch and a headless one, from the live
status file the game writes ($TMPDIR/nanpa2-status-<pid>.json,
src/dev/status.c) about once a second.

  tests/smoke.py BUILD/nanpa2 GAME_DIR [--seconds 35] [--verbose]

Interactive run (real window and presenter, no NANPA2_* variables):
  - exactly one window (SDL and on screen, or the app hidden)
  - the game's frame counter (0x446292) advances
  - non-black content by T=20 s: while the desktop is visible, a frame that
    reached the window; while the window is covered or the screen locked,
    the presenter's offscreen probe (the skipped frame rendered through the
    Metal pipeline and read back)
  - frames presented whenever the desktop was visible
  - no watchdog warnings (a "presents skipped, occluded" one only counts
    when the window was never reported covered)
Headless run (NANPA2_HEADLESS=1: dummy video driver, software presenter):
  no window, the frame counter advances, non-black frames by T=20 s, no
  warnings.

If the screen is locked or the display asleep at the start, the interactive
run is not launched (the window server and GPU setup can block a process
uninterruptibly then); only the headless run is checked.

Exit codes: 0 everything passed with frames verified on screen; 77 (ctest
SKIP_RETURN_CODE) the game checks passed but on-screen presentation could not
be verified (screen locked, display asleep, window covered); 1 a failure,
with the diagnosis. Never waits on a process it cannot kill (a startup hang
is reported with its stage). Uses only the Python standard library.
"""
import argparse
import ctypes
import json
import os
import signal
import subprocess
import sys
import tempfile
import time

SKIP = 77
BLACK = 4  # PRESENT_BLACK_LUMA
STARTUP_S = 15  # the status file must reach stage "running" by then


def status_path(pid):
    return os.path.join(os.environ.get("TMPDIR") or "/tmp", "nanpa2-status-%d.json" % pid)


def clean_env(extra=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("NANPA2_")}
    env.update(extra or {})
    return env


def desktop_state():
    """(locked, asleep): 1 yes, 0 no, -1 unknown (as src/platform/desktop_macos.c)."""
    if sys.platform != "darwin":
        return -1, -1
    try:
        cg = ctypes.CDLL("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
        cf = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
        cg.CGSessionCopyCurrentDictionary.restype = ctypes.c_void_p
        cg.CGMainDisplayID.restype = ctypes.c_uint32
        cg.CGDisplayIsAsleep.argtypes = [ctypes.c_uint32]
        cf.CFStringCreateWithCString.restype = ctypes.c_void_p
        cf.CFStringCreateWithCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
        cf.CFDictionaryGetValue.restype = ctypes.c_void_p
        cf.CFDictionaryGetValue.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        cf.CFBooleanGetValue.argtypes = [ctypes.c_void_p]
        cf.CFRelease.argtypes = [ctypes.c_void_p]
        locked = -1
        d = cg.CGSessionCopyCurrentDictionary()
        if d:
            k = cf.CFStringCreateWithCString(None, b"CGSSessionScreenIsLocked", 0x08000100)
            v = cf.CFDictionaryGetValue(d, k)
            locked = int(bool(v) and bool(cf.CFBooleanGetValue(v)))
            cf.CFRelease(k)
            cf.CFRelease(d)
        asleep = int(bool(cg.CGDisplayIsAsleep(cg.CGMainDisplayID())))
        return locked, asleep
    except (OSError, AttributeError):
        return -1, -1


def proc_state(pid):
    try:
        return subprocess.run(["ps", "-o", "stat=", "-p", str(pid)], capture_output=True, text=True,
                              timeout=5).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return "?"


def stop(p, gently):
    """Stop the child without ever blocking on it. Returns a note if it is
    still alive (state U: the kill is delivered when its system call ends)."""
    if p.poll() is None and gently:
        p.send_signal(signal.SIGTERM)
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            pass
    if p.poll() is None:
        p.kill()
        try:
            p.wait(3)
        except subprocess.TimeoutExpired:
            return "pid %d could not be killed (state %s); it exits once its system call returns" % (
                p.pid, proc_state(p.pid))
    return None


def show(label, d):
    if d.get("stage") != "running":
        print("%s %5.1fs stage %s desktop %s %s" % (label, d["uptime_ms"] / 1000, d.get("stage"),
                                                  d.get("desktop"), d.get("warning")), flush=True)
        return
    pr, g, w, dk = d["present"], d["game"], d["window"], d["desktop"]
    print("%s %5.1fs win %d/%d occl %d hid %d lock %d sleep %d in %d shown %d skip %s luma %d/%d "
          "probe %d frame %d active %d logo %d %s" % (
              label, d["uptime_ms"] / 1000, w["count"], w["os_windows"], w["occluded"],
              dk["app_hidden"], dk["locked"], dk["asleep"], pr["frames_in"], pr["presented"],
              "/".join(str(v) for v in pr["skipped"].values()), pr["luma_in"], pr["luma_shown"],
              pr["probe"]["luma"], g["frame"], g["active_4ac4ec"], d["logo"], d["warning"]), flush=True)


def run(binary, game, seconds, env, verbose, label):
    """Launch, sample the status file each second. Returns a dict: samples,
    log, hang (diagnosis or None), exited, stale, orphan."""
    log = tempfile.TemporaryFile(mode="w+")
    p = subprocess.Popen([binary, "--game", game], env=env, stdout=log, stderr=subprocess.STDOUT)
    path = status_path(p.pid)
    r = {"samples": [], "hang": None, "exited": None, "stale": None, "orphan": None}
    t0 = time.time()
    last = None
    while time.time() - t0 < seconds:
        time.sleep(1)
        if p.poll() is not None:
            r["exited"] = p.returncode
            break
        try:
            with open(path) as f:
                last = json.load(f)
        except (OSError, ValueError):
            pass
        else:
            r["samples"].append(last)
            if verbose:
                show(label, last)
        if time.time() - t0 > STARTUP_S and not any(s.get("stage") == "running" for s in r["samples"]):
            stage = last.get("stage") if last else "before the status file"
            r["hang"] = "startup hang in stage %s after %.0f s (process state %s%s)" % (
                stage, time.time() - t0, proc_state(p.pid),
                "; " + last["warning"] if last and last.get("warning") else "")
            break
    # a startup hang gets SIGKILL only: a pending SIGTERM could turn into
    # SDL_QUIT after the unlock and flash a window
    r["orphan"] = stop(p, gently=not r["hang"])
    log.seek(0)
    r["log"] = log.read()
    if not r["orphan"] and os.path.exists(path):
        r["stale"] = path
    return r


def visible(s):
    return s["window"]["occluded"] == 0 and s["desktop"]["locked"] != 1 and s["desktop"]["asleep"] != 1


def unavailable_why(st):
    why = []
    if any(s["desktop"]["locked"] == 1 for s in st):
        why.append("screen locked")
    if any(s["desktop"]["asleep"] == 1 for s in st):
        why.append("display asleep")
    if any(s["desktop"]["app_hidden"] == 1 for s in st):
        why.append("app hidden")
    if any(s["window"]["occluded"] == 1 for s in st) and not why:
        why.append("window covered")
    return "/".join(why) or "desktop not visible"


def check_run(r, label):
    """Checks common to both runs; returns (errs, running samples)."""
    errs = []
    st = [s for s in r["samples"] if s.get("stage") == "running"]
    if r["hang"]:
        errs.append("%s: %s" % (label, r["hang"]))
    elif not st:
        errs.append("%s: no status file was written" % label)
    if r["exited"] is not None:
        errs.append("%s: the game exited early (code %s)" % (label, r["exited"]))
    if r["stale"]:
        errs.append("%s: status file left behind: %s" % (label, r["stale"]))
    if r["orphan"]:
        errs.append("%s: %s" % (label, r["orphan"]))
    if not st:
        return errs, st
    last = st[-1]
    frames = [s["game"]["frame"] for s in st]
    if len(frames) < 2 or frames[-1] <= frames[0] or last["game"]["frame_still_ms"] > 3000:
        errs.append("%s: the frame counter does not advance (%s)" % (label, frames[-5:]))
    return errs, st


def warned(r):
    keys = []
    for s in r["samples"]:
        for k in s.get("warned", []):
            if k not in keys:
                keys.append(k)
    return keys


def first_warning(r, key):
    for line in r["log"].splitlines():
        if "WARNING:" in line:
            return line.split("WARNING:", 1)[1].strip()
    return key


def check_interactive(r):
    """Returns (errs, unverified reason or None)."""
    errs, st = check_run(r, "interactive")
    if not st:
        return errs, None
    last = st[-1]
    for s in st:
        w, dk = s["window"], s["desktop"]
        on_screen = w["os_windows"] in (1, -1) or (dk["app_hidden"] == 1 and w["os_windows"] == 0)
        if w["count"] != 1 or not on_screen or not (w["shown"] or dk["app_hidden"] == 1):
            errs.append("at %.1f s: %d SDL windows, %d on screen, shown %d, app hidden %d (want exactly one)" % (
                s["uptime_ms"] / 1000, w["count"], w["os_windows"], w["shown"], dk["app_hidden"]))
            break
    # the first second can report the window covered before it is ordered in
    settled = [s for s in st if s["uptime_ms"] >= 3000] or st
    seen = [s for s in settled if visible(s)]
    hidden = [s for s in settled if not visible(s)]
    if seen and last["present"]["presented"] == 0:
        errs.append("no frame reached the window although the desktop was visible")
    by20 = [s for s in st if s["uptime_ms"] <= 20000]
    on_screen_ok = any(visible(s) and s["present"]["luma_shown"] >= BLACK for s in by20)
    # a frame that reached the window (even with the screen locked) or the
    # offscreen probe of a skipped one
    content_ok = any(s["present"]["luma_shown"] >= BLACK or (s["present"]["probe"]["luma"] >= BLACK
                     and 0 <= s["present"]["probe"]["age_ms"] <= 2500) for s in by20)
    unverified = None
    if not on_screen_ok:
        if content_ok:
            unverified = unavailable_why(hidden)
        elif any(not visible(s) for s in by20) and last["present"]["probe"]["count"] == 0:
            errs.append("no non-black frame by T=20 s: nothing was presented (%s) and the presenter has no "
                        "offscreen probe" % unavailable_why(hidden))
        else:
            errs.append("no non-black frame by T=20 s (on screen or in the offscreen probe; last luma in %d, "
                        "shown %d, probe %d)" % (last["present"]["luma_in"], last["present"]["luma_shown"],
                                                  last["present"]["probe"]["luma"]))
    allowed = set()
    if any(s["window"]["occluded"] == 1 for s in st):
        allowed.add("occl")  # presents skipped: correct while covered
    if any(s.get("desktop", {}).get("locked") == 1 for s in r["samples"]):
        allowed.add("init")  # a slow start with the screen locked
    bad = [k for k in warned(r) if k not in allowed]
    if bad:
        errs.append("watchdog warning(s) %s: %s" % (bad, first_warning(r, bad[0])))
    return errs, unverified


def check_headless(r):
    errs, st = check_run(r, "headless")
    if not st:
        return errs
    for s in st:
        w = s["window"]
        if w["shown"] or w["os_windows"] > 0:
            errs.append("headless: a window is on screen (flags %#x, os_windows %d)" % (
                w["flags"], w["os_windows"]))
            break
    if not st[-1]["mode"]["headless"]:
        errs.append("headless: the status file does not record headless mode")
    by20 = [s for s in st if s["uptime_ms"] <= 20000]
    if not any(s["present"]["luma_in"] >= BLACK or s["present"]["probe"]["luma"] >= BLACK for s in by20):
        errs.append("headless: no non-black frame by T=20 s (last luma %d)" % st[-1]["present"]["luma_in"])
    if warned(r):
        errs.append("headless: watchdog warning(s) %s: %s" % (warned(r), first_warning(r, warned(r)[0])))
    return errs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("game")
    ap.add_argument("--seconds", type=float, default=35)
    ap.add_argument("--headless-seconds", type=float, default=24)
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    errs, unverified, inter = [], None, None
    locked, asleep = desktop_state()
    print("desktop at start: locked %d, display asleep %d" % (locked, asleep), flush=True)
    if locked == 1 or asleep == 1:
        unverified = "screen locked" if locked == 1 else "display asleep"
        print("interactive run skipped (%s: the window server and GPU setup can block the game)" % unverified)
    else:
        inter = run(a.binary, a.game, a.seconds, clean_env(), a.verbose, "interactive")
        if inter["hang"] and 1 in desktop_state():
            # locked during the start: not a failure of the game
            unverified = "screen locked during startup (%s)" % inter["hang"]
            errs += [e for e in check_run(inter, "interactive")[0] if inter["hang"] not in e
                     and "could not be killed" not in e]
        else:
            e, unverified = check_interactive(inter)
            errs += e
    head = run(a.binary, a.game, a.headless_seconds, clean_env({"NANPA2_HEADLESS": "1"}), a.verbose, "headless")
    errs += check_headless(head)

    if errs:
        print("SMOKE FAIL")
        for e in errs:
            print("  " + e)
        for name, r in (("interactive", inter), ("headless", head)):
            if not r:
                continue
            if r["samples"]:
                print("last status (%s):\n%s" % (name, json.dumps(r["samples"][-1], indent=2)))
            print("log (%s):\n%s" % (name, "\n".join(r["log"].splitlines()[-20:])))
        return 1
    hl = [s for s in head["samples"] if s.get("stage") == "running"][-1]
    hmsg = "headless: no window, frame counter %d, luma %d, no warnings" % (
        hl["game"]["frame"], hl["present"]["luma_in"])
    st = [s for s in inter["samples"] if s.get("stage") == "running"] if inter else []
    if unverified:
        detail = ""
        if st:
            last = st[-1]
            detail = "interactive: one window, frame counter %d, %d frames presented, %d presents skipped " \
                     "as occluded, offscreen probe luma %d (%d probes), warnings %s; " % (
                         last["game"]["frame"], last["present"]["presented"],
                         last["present"]["skipped"]["occluded"], last["present"]["probe"]["luma"],
                         last["present"]["probe"]["count"], warned(inter) or "none")
        print("SMOKE PASS (desktop %s: on-screen presentation not verified)\n  %s%s" % (unverified, detail, hmsg))
        return SKIP
    last = st[-1]
    print("SMOKE OK: one window, %d frames presented, frame counter %d, luma %d, no warnings; %s" % (
        last["present"]["presented"], last["game"]["frame"], last["present"]["luma_shown"], hmsg))
    return 0


if __name__ == "__main__":
    sys.exit(main())
