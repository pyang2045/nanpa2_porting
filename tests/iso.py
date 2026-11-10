#!/usr/bin/env python3
"""The game from its CD image (src/iso.c), checked against the extracted folder.

  tests/iso.py NANPA2 ISO_CHECK DECODE_ALL GAME_DIR WORK_DIR [IMAGE.iso]

The image is the sixth argument or $NANPA2_ISO; without one (or without the folder) the test is
skipped (exit 77).
1. iso_check: every file under DATA, MUSIC and SPEECH of the folder reads the same bytes from the
   image, whole and through the FILE stream.
2. decode_all on the image prints the same totals as on the folder.
3. From the same seeded save folder (the image has no FLAG files), a recording made on the folder (bot, new game) replays against the image with every checkpoint
   matching (screen, state hash, file opens), and the files the run opened (NANPA2_LOG) and the
   music and voices it started are the same list on both.
"""
import os
import re
import shutil
import subprocess
import sys

SKIP = 77


def sh(args, env=None, timeout=600):
    p = subprocess.run(args, env=env, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def main():
    if len(sys.argv) < 6:
        print(__doc__)
        return 1
    exe, iso_check, decode_all, game, work = sys.argv[1:6]
    iso = sys.argv[6] if len(sys.argv) > 6 and sys.argv[6] else os.environ.get("NANPA2_ISO", "")
    if not iso or not os.path.isfile(iso) or not os.path.isdir(os.path.join(game, "DATA")):
        print("no ISO image or game folder: skipped")
        return SKIP
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)

    rc, out = sh([iso_check, iso, game])
    print(out.strip())
    if rc:
        print("FAIL: iso_check")
        return 1

    rc, a = sh([decode_all, game])
    rc2, b = sh([decode_all, iso])
    print("decode_all folder: %s | image: %s" % (a.strip().splitlines()[-1], b.strip().splitlines()[-1]))
    if rc or rc2 or a != b:
        print("FAIL: decode_all differs")
        return 1

    def run(name, args, extra):
        d = os.path.join(work, name)
        os.makedirs(os.path.join(d, "saves"))
        # the image has no FLAG files (they are inside data1.cab): both runs start from the same
        # copies of the folder's, in the save folder
        for n in os.listdir(game):
            if re.fullmatch(r"FLAG[0-6]", n):
                shutil.copy(os.path.join(game, n), os.path.join(d, "saves", n.lower()))
        env = dict(os.environ, NANPA2_HEADLESS="1", NANPA2_SAVE_DIR=os.path.join(d, "saves"),
                   NANPA2_SPEED="0", NANPA2_CP_TICKS="250", NANPA2_LOG="1")
        for k in [k for k in env if k.startswith(("NANPA2_EXPLORE", "NANPA2_INPUT", "NANPA2_REPLAY", "NANPA2_RECORD"))]:
            del env[k]
        env.update(extra)
        return sh([exe] + args, env)

    rec = os.path.join(work, "a.rec")
    rc, fout = run("folder", ["--game", game, "--record", rec],
                   {"NANPA2_EXPLORE": "31", "NANPA2_EXPLORE_FRAMES": os.environ.get("NANPA2_ISO_FRAMES", "2500")})
    if rc:
        print(fout[-2000:])
        print("FAIL: recording on the folder exited %d" % rc)
        return 1
    rc, iout = run("image", ["--game", iso, "--replay", rec], {})
    if rc or "replay: ok" not in iout:
        print(iout[-2000:])
        print("FAIL: the replay on the image did not match (exit %d)" % rc)
        return 1
    print([l for l in iout.splitlines() if l.startswith("replay: ok")][0])

    def opened(text):
        return [re.sub(r"\s+", " ", l) for l in text.splitlines() if l.startswith(("fopen ", "music ", "voice "))]

    fo, io = opened(fout), opened(iout)
    if fo != io or not fo:
        print("FAIL: the files opened differ (folder %d lines, image %d)" % (len(fo), len(io)))
        return 1
    print("iso: ok (%d opens and sound starts equal)" % len(fo))
    return 0


sys.exit(main())
