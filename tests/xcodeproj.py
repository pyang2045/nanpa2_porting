#!/usr/bin/env python3
"""Drift check between CMakeLists.txt and the iOS Xcode project.

usage: xcodeproj.py SOURCE_DIR

ios/nanpa2.xcodeproj must compile exactly the sources CMake compiles for the game, minus the ones that
are desktop-only, plus the iOS-only ones. Also: the committed project is what tools/gen_xcodeproj.py
writes, and it holds no absolute path, signing team, user name or xcuserdata.
"""
import os
import re
import subprocess
import sys

root = sys.argv[1]
fail = []

# CMake sources that the iOS build does not compile (desktop presenters and dialogs)
IOS_EXCLUDE = {"src/platform/present_sdl.c", "src/platform/desktop_macos.c", "src/platform/dialog_macos.m"}
# compiled by iOS only (CMake never sees them)
IOS_ONLY = {"src/platform/docs_ios.c", "src/platform/dialog_ios.m", "src/platform/orient_ios.m"}

cmake = open(os.path.join(root, "CMakeLists.txt"), encoding="utf-8").read()
cm = set(re.findall(r"src/[A-Za-z0-9_/]+\.(?:c|m)\b", cmake))
cm = {p for p in cm if p.startswith("src/") and not p.startswith("src/tests")}
# the test-only tools in CMake (decode_all, iso_check) use tests/*.c, which the pattern ignores
want = (cm - IOS_EXCLUDE) | IOS_ONLY

pbx_path = os.path.join(root, "ios", "nanpa2.xcodeproj", "project.pbxproj")
pbx = open(pbx_path, encoding="utf-8").read()


def section(name):
    m = re.search(r"/\* Begin %s section \*/\n(.*?)/\* End %s section \*/" % (name, name), pbx, re.S)
    return m.group(1) if m else ""


files = {m.group(1): m.group(2) for m in re.finditer(r"^\t\t(\w+) /\* [^*]* \*/ = \{isa = PBXFileReference;.*?path = ([^;]+);", section("PBXFileReference"), re.M)}
groups = {}
for m in re.finditer(r"^\t\t(\w+)(?: /\*[^*]*\*/)? = \{\n(.*?)^\t\t\};", section("PBXGroup"), re.M | re.S):
    body = m.group(2)
    kids = re.findall(r"^\t\t\t\t(\w+)", body.split("children = (")[1].split(");")[0], re.M)
    p = re.search(r"^\t\t\tpath = ([^;]+);", body, re.M)
    groups[m.group(1)] = (p.group(1).strip('"') if p else "", kids)
paths = {}


def walk(gid, prefix):
    gp, kids = groups[gid]
    base = os.path.normpath(os.path.join(prefix, gp)) if gp else prefix
    for k in kids:
        if k in groups:
            walk(k, base)
        elif k in files:
            paths[k] = os.path.normpath(os.path.join(base, files[k].strip('"')))


main = re.search(r"mainGroup = (\w+)", pbx).group(1)
walk(main, "ios")

build = {m.group(1): m.group(2) for m in re.finditer(r"^\t\t(\w+) /\* [^*]* \*/ = \{isa = PBXBuildFile; fileRef = (\w+);", section("PBXBuildFile"), re.M)}
phase = re.search(r"PBXSourcesBuildPhase;.*?files = \((.*?)\);", section("PBXSourcesBuildPhase"), re.S).group(1)
have = {paths[build[b]] for b in re.findall(r"^\t+(\w+)", phase, re.M)}

for p in sorted(want - have):
    fail.append("CMake compiles %s but ios/nanpa2.xcodeproj does not: add it to SOURCES in tools/gen_xcodeproj.py "
                "and run it (or list it in tests/xcodeproj.py IOS_EXCLUDE if iOS must not build it)" % p)
for p in sorted(have - want):
    fail.append("ios/nanpa2.xcodeproj compiles %s but CMakeLists.txt does not list it (tools/gen_xcodeproj.py SOURCES)" % p)

gen = subprocess.run([sys.executable, os.path.join(root, "tools", "gen_xcodeproj.py"), "--check"],
                     capture_output=True, text=True)
if gen.returncode:
    fail.append("committed Xcode project differs from tools/gen_xcodeproj.py output:\n" + gen.stdout + gen.stderr)

if re.search(r"/(Users|home)/|DEVELOPMENT_TEAM = [A-Z0-9]{4,}", pbx):
    fail.append("project.pbxproj holds an absolute path or a signing team")
if "DEVELOPMENT_TEAM = \"\"" not in pbx:
    fail.append("DEVELOPMENT_TEAM must be empty")
for dirpath, dirs, _ in os.walk(os.path.join(root, "ios")):
    if "xcuserdata" in dirs and "third_party" not in dirpath:
        fail.append("xcuserdata under " + dirpath)

if fail:
    print("\n".join(fail))
    sys.exit(1)
print("xcodeproj: ok (%d sources)" % len(have))
