# 同級生２ native port

A from-scratch reimplementation of the engine of 同級生２ (Doukyuusei 2), Taiwan Traditional
Chinese Windows release (`nanpa2.exe`, 2000), on SDL2, so it runs natively on macOS (Metal),
Linux and Windows. It needs your own copy of the game CD; no game data is in this repository.

## Status

Playable from boot through the full Prologue and into the first town. Working: boot, the
`LOGO.AVI` intro (off by default, `--logo`), name entry, the full Prologue, OPEN1 and walking around the town, save and
load, MP3 music and Chinese voice, the Metal FD Trinitron CRT shader (window and fullscreen), pacing
at 486 speed, and a macOS `.app` bundle. See `NOTES.md` for the dated log.

Tested: an exploration bot has reached 140 of the 470 MES script files, and it found the
script op 0x1f trap (fixed in e3635ff).

Not verified yet: endings, most character routes, and the SPECIAL, TRAVEL and TENNIS parts.

## Build

The build reads your own `nanpa2.exe` (default `game/nanpa2.exe`, see Game data):
`tools/recomp.py` lifts its game code to C in the build folder, with Python and capstone. No
public CI can build it for that reason. The scripts check the tools, name the packages
when something is missing (they never run `sudo`), and pass `--exe`, `--game-dir`,
`--build-dir`, `--release` (default) or `--debug`, `--test` (ctest) and `--backend` (passed as
`-DNANPA2_BACKEND`) through to CMake. Re-running reuses the venv and the build folder.

### macOS (verified)

```sh
brew install sdl2 cmake ninja python
tools/build.sh                 # build/nanpa2; creates tools/.venv with capstone on first run
tools/build.sh --test --app    # plus ctest and build/同級生２.app (this machine only)
```

### Linux (not built or run yet)

```sh
sudo apt install build-essential cmake ninja-build pkg-config libsdl2-dev python3 python3-venv
# Fedora: sudo dnf install gcc cmake ninja-build pkgconf-pkg-config SDL2-devel python3
tools/build.sh --test
```

### Windows (not built or run yet)

In an MSYS2 MINGW64 shell (MinGW-w64 GCC; MSVC is unchecked, see Platforms):

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-pkgconf mingw-w64-x86_64-SDL2 mingw-w64-x86_64-python \
  mingw-w64-x86_64-python-capstone
tools/build_msys2.sh --test
```

It builds `nanpa2-port.exe` (a GUI program, so no console; set `NANPA2_LOG_FILE` for the
log), named so it never replaces the game's own `nanpa2.exe`, and copies it with `SDL2.dll`
and the MinGW runtime DLLs into `build/dist`. Like the `.app`, it holds code lifted from
your `nanpa2.exe`: for your machine only, never share it.

### By hand

```sh
cmake -S . -B build -G Ninja   # -DNANPA2_EXE=... -DNANPA2_PYTHON=... (Python with capstone)
ninja -C build
```

## Game data

Copy the CD contents into `game/` and extract `nanpa2.exe` and `FLAG0`..`FLAG6` from
`data1.cab` there (`unshield x data1.cab`). See `game/README.md`.

## Run

```sh
./build/nanpa2 --game game        # play
./build/nanpa2 --view             # picture viewer: Left/Right, Esc
```

At launch the game shows two openings: `LOGO.AVI`, the Taiwan publisher's movie (15 seconds), then ELF's own animated logo (`start.mes`), followed by the title. The port
skips the publisher movie by default, from the binary and from the `.app`; `--logo` (or
`NANPA2_LOGO=1`) plays it, and a key or click skips it as in the original. `--no-logo` is
still accepted.

Without `--game` the game folder is looked up in this order: `NANPA2_GAME_DIR`; the folder
remembered from an earlier choice; next to the executable or the `.app`, or in a `game` folder
there (a few levels up, so `build/nanpa2` and `build/同級生２.app` find the repository's
`game/`); `./game`. If none of them has `DATA/` and `nanpa2.exe`, a folder picker opens
(macOS) and the choice is remembered in `game-folder.txt` in the save folder
(`~/Library/Application Support/nanpa2/nanpa2/` on macOS, next to the `flag*` save files;
`NANPA2_SAVE_DIR` moves both). A folder that is not the game shows a message box.

### Text font

The game draws its text in MingLiU (細明體) at 15, 24 and 32 pixels. The port reads a font
installed on the system (none is shipped): `--font PATH` (any TrueType file or collection,
e.g. a copy of `mingliu.ttc`), else Apple LiSung Light, a Ming font with hand-made 16 and 24
pixel bitmaps that macOS downloads on demand (it is listed in Font Book before it is
downloaded), else Songti TC Regular, drawn from its outline. The log line `Font:` names the
choice; a second font fills in characters the first lacks (LiSung has no kana or ～).
`NANPA2_TEXT=outline` ignores bitmaps and `NANPA2_TEXT=threshold` restores the old plain
outline threshold, for comparison.

### macOS app (local use only)

```sh
ninja -C build nanpa2_app          # or configure with -DNANPA2_APP_BUNDLE=ON
open build/同級生２.app             # or double-click it in Finder; it can be moved anywhere
```

`build/同級生２.app` holds the binary and the SDL libraries it needs, so it runs without
Homebrew's paths: Homebrew's SDL2 is sdl2-compat, which loads SDL3 at run time, so
`tools/bundle_macos.sh` copies both into `Contents/Frameworks`, rewrites the load paths to
`@executable_path`/`@loader_path` and signs everything ad hoc. It runs on the macOS version
the libraries were built for and later (`LSMinimumSystemVersion` is taken from them; with
current Homebrew bottles that is macOS 26). It contains no game data, but **the binary
contains code translated from your own `nanpa2.exe`: do not share or upload the app.** On
the first start choose the game folder in the picker (or put the app next to a `game`
folder).

Started from Finder there is no terminal, so the log (stdout, stderr, `SDL_Log`) goes to
`nanpa2.log` in the save folder, and the previous run's to `nanpa2.previous.log`:

```sh
cat ~/Library/Application\ Support/nanpa2/nanpa2/nanpa2.log
```

`NANPA2_LOG_FILE=PATH` sends the log to a file from any launch. To try the app without
touching your saves: `open -n build/同級生２.app --env NANPA2_SAVE_DIR=/tmp/x --env
NANPA2_GAME_DIR=$PWD/game`. To get the picker again, delete `game-folder.txt`. A launch from
Finder also writes the status file (below); `$TMPDIR` is the same for Finder and Terminal.

The window is raised at startup and the game keeps running (music too) when it is in the
background; `--pause-inactive` restores the original behaviour of stopping it while the
window has no focus.

### Diagnosing a running instance

While the game runs it writes `$TMPDIR/nanpa2-status-<pid>.json` about once a second
(removed on exit; files of killed instances are cleaned up by the next start):

```sh
cat $TMPDIR/nanpa2-status-*.json
```

It is written from before `SDL_Init`: `stage` names the startup step reached (`sdl_init`,
`window`, `present_init`, `game_init`, then `running`), so an instance stuck in the video
setup still shows where (and a `WARNING: startup stuck in stage …`). It holds the pid and start time, the desktop
(screen locked, display asleep, app hidden), the mode (headless, scripted, every `NANPA2_*` variable
set), the window (SDL and on-screen window counts, flags, size, drawable size, focus,
occlusion), the cursor (the game's ShowCursor count, host cursor shown, pointer over the
image), the presenter (frames given and presented, presents skipped as occluded / no
drawable / size 0, worst `nextDrawable` wait, mean luminance of the last frame, how long the
screen has been black, and the offscreen probe: while presents are skipped because the
window is covered, the skipped frame is rendered through the Metal pipeline offscreen, CRT
off, about once a second and its luminance recorded), the game (frame counter 0x446292 and how long it has stood still,
active flag 0x4ac4ec, timer, last poll pc, CPU MIPS), the logo movie, music and voice. A
watchdog logs one `WARNING:` line, and fills `warning`, when the screen stays black or
nothing reaches the window for 8 s, the frame counter stops while the window is visible, the
main thread stalls, or a headless run shows a window; it names the likely cause (e.g.
`presents skipped, occluded=…`, `timer stopped: 0x4ac4ec=0`, `stuck polling at pc …`);
`warning_key` and `warned` (every key so far) name the causes for scripts.

`NANPA2_HEADLESS=1` (scripted and smoke test runs) uses SDL's dummy video driver and the
presenter's software path (no window server, no GPU): nothing appears on the desktop, and it
runs while the screen is locked. Frames are kept in memory; screenshots
(`NANPA2_SHOT_PRESENT`) are the plain 640x480 picture without the CRT.

Tests while the screen is locked: use headless. A windowed launch can then hang in startup
(SDL_Init's display link or CAMetalLayer `setDevice`, waiting in `IOServiceOpen` in process
state U, unkillable until the unlock); the status file shows the stage.

### Smoke test

```sh
ctest --test-dir build -R smoke --output-on-failure   # or: tests/smoke.py build/nanpa2 game
```

First reads the desktop state. If the screen is visible, a plain interactive launch (real
window and presenter, no `NANPA2_*` variables) runs 35 s and must show: exactly one window,
the frame counter advancing, a non-black frame by 20 s, frames presented, no watchdog
warnings. If the window is covered (or the screen gets locked) during that run, frames
cannot reach the screen; the presenter's offscreen probe then has to show a non-black frame
by 20 s instead, and the `occl` warning is expected. If the screen is locked or the display
asleep at the start, the interactive run is skipped. Then a 24 s `NANPA2_HEADLESS=1` run must
show no window, the frame counter advancing, non-black frames by 20 s and no warnings.

Results: `SMOKE OK` (exit 0) when everything passed with frames seen on screen;
`SMOKE PASS (desktop …: on-screen presentation not verified)` (exit 77, shown by ctest as
Skipped) when the game checks passed but nobody could see the screen; `SMOKE FAIL` (exit 1)
with the diagnosis and the last status otherwise. A startup hang is reported with its stage
and the test never waits on a process it cannot kill. Set the game folder with
`-DNANPA2_GAME_DIR=...`.

## Platforms

macOS (Intel, macOS 26; Metal, and SDL_Renderer with `-DNANPA2_SDL_RENDERER=ON`) is the only
platform built and run so far. The code is meant to build on Linux and Windows with SDL2,
but neither has been built or run. Known points:

- Linux: CMake links libm. Big5 goes through iconv (`CP950`, falling back to `BIG5`, which
  lacks Microsoft's extensions); whether glibc offers `CP950` is unchecked. Fonts come from
  `/usr/share/fonts` (AR PL UMing, Noto Serif CJK) or `--font`. Game file names are matched
  case-insensitively (`src/fsys.c`) and save files are always lower case, so a
  case-sensitive file system is fine. No folder picker (`src/platform/dialog_none.c`): use
  `--game`, `NANPA2_GAME_DIR` or a `game` folder next to the binary. The desktop state
  (screen locked) is unknown there, so the smoke test cannot skip on it.
- Windows: Big5 via `MultiByteToWideChar(950)`, MingLiU from `C:\Windows\Fonts`; the status
  file goes to `%TEMP%`; the viewer lists `DATA\*.GP4` with `_findfirst`; stale status files
  of killed instances are not removed. Unchecked: MSVC (the generated `recomp.c` is plain C11
  with GCC/Clang flags, so MinGW-w64 or clang-cl is the safer bet), non-ASCII paths (the C
  runtime's `fopen` takes the ANSI code page) and no folder picker. The target is a
  `WIN32_EXECUTABLE` named `nanpa2-port.exe` (SDL2main supplies WinMain), so no console
  opens; the log goes to `NANPA2_LOG_FILE`.
- Both: the presenter is `src/platform/present_sdl.c` (SDL_Renderer); the status file works,
  the smoke test's desktop checks are macOS only.
