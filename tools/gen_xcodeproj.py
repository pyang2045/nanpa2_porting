#!/usr/bin/env python3
"""Generate ios/nanpa2.xcodeproj (the iOS build, README "iPad / iPhone").

This script is the source of truth for the project files: edit it, run it, commit the result.
Output is deterministic (object IDs are hashed from stable names). tests/xcodeproj.py checks that the
committed project is what this script writes and that it compiles the sources CMake compiles.

    python3 tools/gen_xcodeproj.py            write ios/nanpa2.xcodeproj/...
    python3 tools/gen_xcodeproj.py --check    exit 1 if the committed files differ
"""
import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJ = os.path.join(ROOT, "ios", "nanpa2.xcodeproj")

# Paths are relative to src/. Keep in step with CMakeLists.txt (tests/xcodeproj.py fails when they drift):
# the portable sources, then the ones the iOS build picks (present_metal.m, dialog_sdl.c, docs_ios.c).
SOURCES = """
main.c launch.c lzh.c gp4.c fsys.c iso.c trap.c hooks.c hash.c cinepak.c
platform/files.c platform/window.c platform/clock.c platform/surface.c platform/text.c platform/logo.c
platform/textlayer.c platform/textraster.c platform/font_apple.c
platform/sound.c platform/present_metal.m platform/dialog_sdl.c platform/docs_ios.c
dev/dev.c dev/replay.c dev/explore.c dev/status.c
game/ctl.c game/expr.c game/vm.c game/ops.c game/ops_sys.c game/ops_vm.c game/loop.c game/text.c
game/anim.c game/file.c game/startup.c game/map.c game/map_draw.c game/map_move.c game/map_path.c
game/map_sprite.c game/map_pump.c game/screen.c game/wipe.c game/state_view.c game/state.c game/timer.c
""".split()

# Headers shown in the Xcode navigator (not built).
HEADERS = sorted(
    os.path.relpath(os.path.join(d, f), os.path.join(ROOT, "src"))
    for d, _, fs in os.walk(os.path.join(ROOT, "src")) for f in fs if f.endswith(".h")
)
TP_HEADERS = sorted(f for f in os.listdir(os.path.join(ROOT, "third_party")) if f.endswith(".h"))

SDL_XCF = "third_party/SDL3.xcframework"  # built by tools/fetch_sdl.sh, gitignored

# (name, weak): what SDL's iOS build needs (sdl3-config --static-libs) plus ours.
FRAMEWORKS = [("AudioToolbox", 0), ("AVFoundation", 0), ("CoreAudio", 0), ("CoreBluetooth", 0),
              ("CoreFoundation", 0), ("CoreGraphics", 0), ("CoreHaptics", 1), ("CoreMotion", 0), ("CoreText", 0),
              ("CoreVideo", 0), ("Foundation", 0), ("GameController", 1), ("Metal", 0),
              ("OpenGLES", 0), ("QuartzCore", 0), ("UIKit", 0)]


def oid(name):
    return hashlib.md5(name.encode()).hexdigest()[:24].upper()


def q(s):
    s = str(s)
    if s and re.fullmatch(r"[A-Za-z0-9_./]+", s):
        return s
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def keyorder(k):
    return (k != "isa", k)


def val(v, ind):
    t = "\t" * ind
    if isinstance(v, dict):
        out = "{\n"
        for k in sorted(v, key=keyorder):
            out += f"{t}\t{q(k)} = {val(v[k], ind + 1)};\n"
        return out + t + "}"
    if isinstance(v, list):
        if not v:
            return "(\n" + t + ")"
        out = "(\n"
        for x in v:
            out += f"{t}\t{val(x, ind + 1)},\n"
        return out + t + ")"
    return q(v)


def flat(v):
    """one-line form Xcode uses for file references and build files"""
    return "{" + "".join(f"{q(k)} = {q(x)}; " for k, x in sorted(v.items(), key=lambda kv: keyorder(kv[0]))) + "}"


objs = {}  # id -> (comment, dict, oneline)


def add(key, comment, d, one=False):
    i = oid(key)
    objs[i] = (comment, d, one)
    return i


def ftype(p):
    return {".c": "sourcecode.c.c", ".m": "sourcecode.c.objc", ".h": "sourcecode.c.h"}[os.path.splitext(p)[1]]


def file_ref(rel, base):
    return add("ref:" + base + rel, os.path.basename(rel), {
        "isa": "PBXFileReference", "fileEncoding": "4", "lastKnownFileType": ftype(rel),
        "path": os.path.basename(rel), "sourceTree": "<group>"}, True)


# ---- file references and groups ----
refs = {}
src_children = {}
for rel in sorted(set(SOURCES) | set(HEADERS)):
    refs[rel] = file_ref(rel, "src/")
    src_children.setdefault(os.path.dirname(rel), []).append((os.path.basename(rel), refs[rel]))

sub_groups = []
for d in sorted(x for x in src_children if x):
    sub_groups.append(add("group:src/" + d, d, {
        "isa": "PBXGroup", "children": [i for _, i in sorted(src_children[d])], "name": d, "path": d,
        "sourceTree": "<group>"}))
src_group = add("group:src", "src", {
    "isa": "PBXGroup", "children": sub_groups + [i for _, i in sorted(src_children.get("", []))],
    "name": "src", "path": "../src", "sourceTree": "<group>"})

tp_group = add("group:third_party", "third_party", {
    "isa": "PBXGroup", "children": [file_ref(h, "third_party/") for h in TP_HEADERS],
    "name": "third_party", "path": "../third_party", "sourceTree": "<group>"})

plist_ref = add("ref:Info.plist", "Info.plist", {
    "isa": "PBXFileReference", "lastKnownFileType": "text.plist.xml", "path": "Info.plist",
    "sourceTree": "<group>"}, True)
assets_ref = add("ref:Assets.xcassets", "Assets.xcassets", {
    "isa": "PBXFileReference", "lastKnownFileType": "folder.assetcatalog", "path": "Assets.xcassets",
    "sourceTree": "<group>"}, True)
# SDL3.xcframework (static libSDL3 + SDL3main for device and simulator): tools/fetch_sdl.sh builds it
sdl_xcf = add("ref:SDL3.xcframework", "SDL3.xcframework", {
    "isa": "PBXFileReference", "lastKnownFileType": "wrapper.xcframework", "path": SDL_XCF,
    "sourceTree": "<group>"}, True)

fw_refs = []
for n, weak in FRAMEWORKS:
    fw_refs.append((n, weak, add("ref:fw:" + n, n + ".framework", {
        "isa": "PBXFileReference", "lastKnownFileType": "wrapper.framework", "name": n + ".framework",
        "path": f"System/Library/Frameworks/{n}.framework", "sourceTree": "SDKROOT"}, True)))
iconv_ref = add("ref:libiconv", "libiconv.tbd", {
    "isa": "PBXFileReference", "lastKnownFileType": "sourcecode.text-based-dylib-definition",
    "name": "libiconv.tbd", "path": "usr/lib/libiconv.tbd", "sourceTree": "SDKROOT"}, True)
fw_group = add("group:Frameworks", "Frameworks", {
    "isa": "PBXGroup", "children": [i for _, _, i in fw_refs] + [iconv_ref], "name": "Frameworks",
    "sourceTree": "<group>"})

app_ref = add("ref:nanpa2.app", "nanpa2.app", {
    "isa": "PBXFileReference", "explicitFileType": "wrapper.application", "includeInIndex": "0",
    "path": "nanpa2.app", "sourceTree": "BUILT_PRODUCTS_DIR"}, True)
products = add("group:Products", "Products", {
    "isa": "PBXGroup", "children": [app_ref], "name": "Products", "sourceTree": "<group>"})

main_group = add("group:main", "", {
    "isa": "PBXGroup", "children": [src_group, tp_group, plist_ref, assets_ref, sdl_xcf, fw_group, products],
    "sourceTree": "<group>"})

# ---- build files and phases ----
def bf(key, comment, d):
    return add("bf:" + key, comment, dict(d, isa="PBXBuildFile"), True)


# the text layer's glyph rasteriser is compiled -O2 whatever the configuration (CMakeLists.txt does the same)
OPT_SOURCES = ("platform/textlayer.c", "platform/textraster.c")
src_bfs = [bf("src:" + s, f"{os.path.basename(s)} in Sources",
              dict({"fileRef": refs[s]}, **({"settings": "@O2@"} if s in OPT_SOURCES else {})))
           for s in sorted(SOURCES)]
fw_build = []
for n, weak, i in fw_refs:
    d = {"fileRef": i}
    if weak:
        d["settings"] = "@WEAK@"
    fw_build.append(bf("fw:" + n, n + ".framework in Frameworks", d))
iconv_bf = bf("fw:iconv", "libiconv.tbd in Frameworks", {"fileRef": iconv_ref})
sdl_bf = bf("fw:sdl", "SDL3.xcframework in Frameworks", {"fileRef": sdl_xcf})
assets_bf = bf("res:assets", "Assets.xcassets in Resources", {"fileRef": assets_ref})

sources_phase = add("phase:sources", "Sources", {
    "isa": "PBXSourcesBuildPhase", "buildActionMask": "2147483647", "files": src_bfs,
    "runOnlyForDeploymentPostprocessing": "0"})
fw_phase = add("phase:frameworks", "Frameworks", {
    "isa": "PBXFrameworksBuildPhase", "buildActionMask": "2147483647",
    "files": [sdl_bf] + fw_build + [iconv_bf], "runOnlyForDeploymentPostprocessing": "0"})
res_phase = add("phase:resources", "Resources", {
    "isa": "PBXResourcesBuildPhase", "buildActionMask": "2147483647", "files": [assets_bf],
    "runOnlyForDeploymentPostprocessing": "0"})

# ---- build settings ----
FLAGS = ["-Wall", "-Wextra", "-Wno-unused-parameter",
         # no build paths in __FILE__ or debug info (the CMake build did the same)
         "-ffile-prefix-map=$(SRCROOT)/..=.", "-ffile-prefix-map=$(SRCROOT)=ios"]
project_common = {
    "ALWAYS_SEARCH_USER_PATHS": "NO",
    "CLANG_ENABLE_MODULES": "NO",
    "CLANG_ENABLE_OBJC_ARC": "YES",
    "GCC_C_LANGUAGE_STANDARD": "c11",
    "GCC_WARN_64_TO_32_BIT_CONVERSION": "NO",  # CMake does not enable it either
    "IPHONEOS_DEPLOYMENT_TARGET": "13.0",
    "SDKROOT": "iphoneos",
    "SUPPORTED_PLATFORMS": "iphoneos iphonesimulator",
    "TARGETED_DEVICE_FAMILY": "1,2",
}
proj_debug = dict(project_common, DEBUG_INFORMATION_FORMAT="dwarf", GCC_OPTIMIZATION_LEVEL="0",
                  ONLY_ACTIVE_ARCH="YES")
proj_release = dict(project_common, DEBUG_INFORMATION_FORMAT="dwarf", GCC_OPTIMIZATION_LEVEL="3",
                    VALIDATE_PRODUCT="YES", GCC_PREPROCESSOR_DEFINITIONS=["NDEBUG=1", "$(inherited)"])
target_common = {
    "ASSETCATALOG_COMPILER_APPICON_NAME": "AppIcon",
    "CODE_SIGN_STYLE": "Automatic",
    "CURRENT_PROJECT_VERSION": "1",
    "DEVELOPMENT_TEAM": "",
    "GCC_NO_COMMON_BLOCKS": "YES",
    "GENERATE_INFOPLIST_FILE": "NO",
    "HEADER_SEARCH_PATHS": ["$(SRCROOT)/../src", "$(SRCROOT)/../third_party"],
    "INFOPLIST_FILE": "Info.plist",
    "MARKETING_VERSION": "0.1",
    "OTHER_CFLAGS": FLAGS,
    "PRODUCT_BUNDLE_IDENTIFIER": "local.nanpa2",
    "PRODUCT_NAME": "nanpa2",
}
cfg_proj = {n: add("cfg:proj:" + n, n, {"isa": "XCBuildConfiguration", "buildSettings": s, "name": n})
            for n, s in (("Debug", proj_debug), ("Release", proj_release))}
cfg_tgt = {n: add("cfg:tgt:" + n, n, {"isa": "XCBuildConfiguration", "buildSettings": target_common, "name": n})
           for n in ("Debug", "Release")}
cl_proj = add("cl:proj", 'Build configuration list for PBXProject "nanpa2"', {
    "isa": "XCConfigurationList", "buildConfigurations": [cfg_proj["Debug"], cfg_proj["Release"]],
    "defaultConfigurationIsVisible": "0", "defaultConfigurationName": "Release"})
cl_tgt = add("cl:tgt", 'Build configuration list for PBXNativeTarget "nanpa2"', {
    "isa": "XCConfigurationList", "buildConfigurations": [cfg_tgt["Debug"], cfg_tgt["Release"]],
    "defaultConfigurationIsVisible": "0", "defaultConfigurationName": "Release"})

target = add("target:nanpa2", "nanpa2", {
    "isa": "PBXNativeTarget", "buildConfigurationList": cl_tgt,
    "buildPhases": [sources_phase, fw_phase, res_phase], "buildRules": [], "dependencies": [],
    "name": "nanpa2", "productName": "nanpa2", "productReference": app_ref,
    "productType": "com.apple.product-type.application"})
project = add("project", "Project object", {
    "isa": "PBXProject", "attributes": {"BuildIndependentTargetsInParallel": "1", "LastUpgradeCheck": "1600"},
    "buildConfigurationList": cl_proj, "compatibilityVersion": "Xcode 14.0", "developmentRegion": "zh_TW",
    "hasScannedForEncodings": "0", "knownRegions": ["zh-Hant", "en", "Base"], "mainGroup": main_group,
    "productRefGroup": products, "projectDirPath": "",
    "projectRoot": "", "targets": [target]})

SECTIONS = ["PBXBuildFile", "PBXFileReference", "PBXFrameworksBuildPhase", "PBXGroup",
            "PBXNativeTarget", "PBXProject", "PBXResourcesBuildPhase",
            "PBXSourcesBuildPhase", "XCBuildConfiguration", "XCConfigurationList"]


def render():
    out = "// !$*UTF8*!\n{\n\tarchiveVersion = 1;\n\tclasses = {\n\t};\n\tobjectVersion = 56;\n\tobjects = {\n"
    for sec in SECTIONS:
        items = sorted((i, v) for i, v in objs.items() if v[1]["isa"] == sec)
        if not items:
            continue
        out += f"\n/* Begin {sec} section */\n"
        for i, (comment, d, one) in items:
            c = f" /* {comment} */" if comment else ""
            if one:
                weak = d.get("settings") == "@WEAK@"
                opt = d.get("settings") == "@O2@"
                s = flat({k: x for k, x in d.items() if x not in ("@WEAK@", "@O2@")})
                if opt:
                    s = s[:-1] + 'settings = {COMPILER_FLAGS = "-O2"; }; }'
                if weak:
                    s = s[:-1] + "settings = {ATTRIBUTES = (Weak, ); }; }"
                out += f"\t\t{i}{c} = {s};\n"
            else:
                out += f"\t\t{i}{c} = {val(d, 2)};\n"
        out += f"/* End {sec} section */\n"
    out += f"\t}};\n\trootObject = {project} /* Project object */;\n}}\n"
    return out


SCHEME = """<?xml version="1.0" encoding="UTF-8"?>
<Scheme
   LastUpgradeVersion = "1600"
   version = "1.7">
   <BuildAction
      parallelizeBuildables = "YES"
      buildImplicitDependencies = "YES">
      <BuildActionEntries>
         <BuildActionEntry
            buildForTesting = "NO"
            buildForRunning = "YES"
            buildForProfiling = "YES"
            buildForArchiving = "YES"
            buildForAnalyzing = "YES">
            <BuildableReference
               BuildableIdentifier = "primary"
               BlueprintIdentifier = "%(t)s"
               BuildableName = "nanpa2.app"
               BlueprintName = "nanpa2"
               ReferencedContainer = "container:nanpa2.xcodeproj">
            </BuildableReference>
         </BuildActionEntry>
      </BuildActionEntries>
   </BuildAction>
   <TestAction
      buildConfiguration = "Debug"
      selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB"
      selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB"
      shouldUseLaunchSchemeArgsEnv = "YES">
   </TestAction>
   <LaunchAction
      buildConfiguration = "Release"
      selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB"
      selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB"
      launchStyle = "0"
      useCustomWorkingDirectory = "NO"
      ignoresPersistentStateOnLaunch = "NO"
      debugDocumentVersioning = "YES"
      debugServiceExtension = "internal"
      allowLocationSimulation = "YES">
      <BuildableProductRunnable
         runnableDebuggingMode = "0">
         <BuildableReference
            BuildableIdentifier = "primary"
            BlueprintIdentifier = "%(t)s"
            BuildableName = "nanpa2.app"
            BlueprintName = "nanpa2"
            ReferencedContainer = "container:nanpa2.xcodeproj">
         </BuildableReference>
      </BuildableProductRunnable>
   </LaunchAction>
   <ProfileAction
      buildConfiguration = "Release"
      shouldUseLaunchSchemeArgsEnv = "YES"
      savedToolIdentifier = ""
      useCustomWorkingDirectory = "NO"
      debugDocumentVersioning = "YES">
   </ProfileAction>
   <AnalyzeAction
      buildConfiguration = "Debug">
   </AnalyzeAction>
   <ArchiveAction
      buildConfiguration = "Release"
      revealArchiveInOrganizer = "YES">
   </ArchiveAction>
</Scheme>
""" % {"t": target}

WORKSPACE = """<?xml version="1.0" encoding="UTF-8"?>
<Workspace
   version = "1.0">
   <FileRef
      location = "self:">
   </FileRef>
</Workspace>
"""

FILES = {
    "project.pbxproj": render(),
    "project.xcworkspace/contents.xcworkspacedata": WORKSPACE,
    "xcshareddata/xcschemes/nanpa2.xcscheme": SCHEME,
}


def main():
    check = "--check" in sys.argv
    bad = False
    for rel, text in FILES.items():
        p = os.path.join(PROJ, rel)
        if check:
            if not os.path.exists(p) or open(p, encoding="utf-8").read() != text:
                print("differs:", os.path.relpath(p, ROOT))
                bad = True
        else:
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w", encoding="utf-8") as f:
                f.write(text)
    if check and bad:
        print("run: python3 tools/gen_xcodeproj.py")
        sys.exit(1)


main()
