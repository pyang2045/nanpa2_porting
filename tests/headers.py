#!/usr/bin/env python3
"""Header hygiene (no game data needed).

usage: headers.py SRC_DIR CC [compiler flags...]

1. Every header in src/ compiles on its own (a TU holding only that #include), so it includes what
   it uses.
2. Layering: nothing outside src/dev/ and src/main.c includes a dev/ header (platform and game code
   reach the dev layer through src/hooks.h), and src/game/ includes no SDL.
"""
import os, re, subprocess, sys, tempfile

src, cc, flags = sys.argv[1], sys.argv[2], sys.argv[3:]
bad = 0
files = []
for d, _, fs in os.walk(src):
    for f in fs:
        if f.endswith((".h", ".c", ".m")):
            files.append(os.path.join(d, f))

for p in sorted(files):
    rel = os.path.relpath(p, src)
    if not p.endswith(".h"):
        continue
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as t:
        t.write('#include "%s"\n' % rel)
    r = subprocess.run([cc, "-fsyntax-only", "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter",
                        "-I", src, "-I", os.path.join(src, "..", "third_party")] + flags + [t.name],
                       capture_output=True, text=True)
    os.unlink(t.name)
    if r.returncode or r.stderr.strip():
        bad += 1
        print("header %s does not stand alone:\n%s" % (rel, r.stderr[:600]))

inc = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
for p in sorted(files):
    rel = os.path.relpath(p, src)
    for m in inc.findall(open(p, errors="replace").read()):
        if m.startswith("dev/") and not (rel.startswith("dev/") or rel == "main.c"):
            bad += 1
            print("%s includes %s: use src/hooks.h" % (rel, m))
        if rel.startswith("game/") and (m.startswith("SDL")):
            bad += 1
            print("%s includes %s: game code never includes SDL" % (rel, m))

print("headers: %s" % ("FAIL" if bad else "ok"))
sys.exit(1 if bad else 0)
