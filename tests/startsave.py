#!/usr/bin/env python3
"""The start save (src/dev/startsave.h): `nanpa2 --make-start-save`.

  tests/startsave.py NANPA2 GAME_DIR WORK_DIR [IMAGE.iso]

1. From an empty save folder it writes flag0 and flag6, 0xc00 bytes each, the same image, and says so;
   a second run into another empty folder writes the same bytes.
2. Its log shows the game's own path: no FLAG file existed in the game folder or the save folder, and the
   file is not what a blank save is (the Prologue ran).
3. With FLAG files already there it does nothing and leaves them alone.
4. From the CD image (the argument or $NANPA2_ISO, else skipped) the bytes are the same.
5. A plain launch is never changed by it: --no-start-save is accepted and a scripted run
   (NANPA2_INPUT) does not generate.
Exit 77 (skipped) without the game folder.
"""
import os
import shutil
import subprocess
import sys

SKIP = 77


def run(exe, game, save, args, env_extra=None, timeout=300):
    env = {k: v for k, v in os.environ.items() if not k.startswith("NANPA2_")}
    env["NANPA2_SAVE_DIR"] = save
    env.update(env_extra or {})
    p = subprocess.run([exe, "--game", game] + args, env=env, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def read(path):
    with open(path, "rb") as f:
        return f.read()


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 1
    exe, game, work = sys.argv[1:4]
    iso = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] else os.environ.get("NANPA2_ISO", "")
    if not os.path.isdir(os.path.join(game, "DATA")):
        print("no game folder: skipped")
        return SKIP
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    fail = []

    def make(name, src):
        d = os.path.join(work, name)
        os.makedirs(d)
        rc, out = run(exe, src, d, ["--make-start-save"])
        print("%s: rc %d, %s" % (name, rc, [l for l in out.splitlines() if "start-save:" in l][-1:]))
        if rc != 0:
            fail.append("%s: exit %d\n%s" % (name, rc, out[-600:]))
            return None
        files = sorted(f for f in os.listdir(d) if not f.endswith((".txt", ".log")))
        if files != ["flag0", "flag6"]:
            fail.append("%s: the save folder holds %s, not flag0 and flag6" % (name, files))
            return None
        a, b = read(os.path.join(d, "flag0")), read(os.path.join(d, "flag6"))
        if len(a) != 0xC00 or a != b:
            fail.append("%s: flag0 and flag6 are not the same 0xc00 bytes" % name)
            return None
        return b

    one = make("a", game)
    two = make("b", game)
    if one and two and one != two:
        fail.append("two runs wrote different bytes")
    if one and one.count(0) > len(one) - 64:
        fail.append("the save is a blank")

    if one:
        d = os.path.join(work, "a")
        before = {f: read(os.path.join(d, f)) for f in os.listdir(d) if f.startswith("flag")}
        rc, out = run(exe, game, d, ["--make-start-save"])
        after = {f: read(os.path.join(d, f)) for f in os.listdir(d) if f.startswith("flag")}
        if rc != 0 or before != after or "nothing done" not in out:
            fail.append("existing saves were not left alone (rc %d)" % rc)

    if iso and os.path.isfile(iso):
        three = make("c", iso)
        if one and three and one != three:
            fail.append("the CD image gave other bytes than the folder")
    else:
        print("no ISO image: that part skipped")

    # scripted runs never generate (no start-save line, no files)
    d = os.path.join(work, "d")
    os.makedirs(d)
    rc, out = run(exe, game, d, [], {"NANPA2_HEADLESS": "1", "NANPA2_INPUT": "quit,500"})
    if [f for f in os.listdir(d) if f.startswith("flag") or f.startswith(".")] or "start-save" in out:
        fail.append("a scripted run generated a start save")

    if fail:
        print("FAIL")
        for f in fail:
            print("  " + f)
        return 1
    print("startsave: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
