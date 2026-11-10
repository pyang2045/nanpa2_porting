# 同級生２ native port

A from-scratch reimplementation of the engine of 同級生２ (Doukyuusei 2), Taiwan Traditional
Chinese Windows release (`nanpa2.exe`, 2000), on SDL2, so it runs natively on macOS (Metal),
Linux and Windows. It needs your own copy of the game CD; no game data is in this repository.

## Status

Playable from boot through the full Prologue and into the first town. Working: boot, the
`LOGO.AVI` intro (off by default, `--logo`), name entry, the full Prologue, OPEN1 and walking around the town, save and
load, MP3 music and Chinese voice, the Metal FD Trinitron CRT shader (window and fullscreen), pacing
at the original 486 speed (`--speed`), and a macOS `.app` bundle. See `NOTES.md` for the dated log.

Tested: an exploration bot has reached 140 of the 470 MES script files, and it found the
script op 0x1f trap (fixed in e3635ff).

Not verified yet: endings, most character routes, and the SPECIAL, TRAVEL and TENNIS parts.

## Build

The engine is plain C on SDL2: the game logic (`src/game`) runs on its own state and calls a small platform
API (`src/platform`: files, surfaces, text, window, sound); no instruction of `nanpa2.exe` runs and
nothing emulates the PC it was written for (see "Architecture"). Nothing is read from the exe: the start
values of the game state are defined in code (see "Game data"). The build needs a C11 compiler, CMake, Ninja
and SDL2 (Python 3 for the tests), nothing else, and the binary and `.app` hold nothing of the game (ctest
`no_game_bytes`): this is the build to share, like xsystem4, the player supplies the game files. The engine
assumes a 64-bit little-endian host (`src/game/state.h` refuses to build otherwise); the game state and the
save files have a fixed-width layout, with no dependence on the size of `long` or of a pointer.

The scripts check the tools, name the packages when something is missing (they never run `sudo`), and pass
`--game-dir`, `--build-dir`, `--release` (default) or `--debug` and `--test` (ctest) through to CMake.
Re-running reuses the build folder.

### macOS (verified)

```sh
brew install sdl2 cmake ninja python
tools/build.sh                 # build/nanpa2
tools/build.sh --test --app    # plus ctest and build/同級生２.app
```

### iPad / iPhone (iOS simulator verified, device untested)

```sh
tools/build_ios.sh                                  # build-ios/sim/nanpa2.app (simulator, ad-hoc signed)
xcrun simctl boot "iPad Pro 11-inch (M4)" && xcrun simctl install booted build-ios/sim/nanpa2.app
xcrun simctl launch booted local.nanpa2
DEVELOPMENT_TEAM=ABCDE12345 tools/build_ios.sh --device   # a real device, signed with your team ID
```

Needs Xcode, CMake and Ninja. The script downloads the official SDL2 release source into `build-ios/`
(gitignored) and builds it statically for the same SDK. A device build uses the Xcode generator and your
Apple team ID (a free personal team works; set `NANPA2_APP_ID` to a bundle identifier of your own), then
install `build-ios/device/Release-iphoneos/nanpa2.app` from Xcode or `xcrun devicectl device install app`.
The app (display name 同級生２, landscape, full screen) holds none of the game. Copy your game folder (the CD
contents plus FLAG0..FLAG6, with DATA inside) into the app's Documents, either as a folder named `game` or
its contents directly: with Finder (iPad connected by USB, Files tab, drag onto the app) or in the Files app
(On My iPad > 同級生２). Without it the app shows a bilingual message; relaunch after copying. In the
simulator: `xcrun simctl get_app_container booted local.nanpa2 data`, then copy into `Documents/game`.
Saves and the log (`nanpa2.log`) are in the app's Library/Application Support.

It presents through the same Metal CRT presenter as macOS (`NANPA2_SDL_RENDERER=ON` selects the
SDL_Renderer one). Touch: one-finger tap = left click, hold or drag = left button held, two-finger tap =
right click (cancel/menu), three-finger tap = next display mode (F12). Touch positions go through the
presenter's letterbox. Going to the background pauses the music and the render loop.

### Linux (not built or run yet)

```sh
sudo apt install build-essential cmake ninja-build pkg-config libsdl2-dev python3
# Fedora: sudo dnf install gcc cmake ninja-build pkgconf-pkg-config SDL2-devel python3
tools/build.sh --test
```

### Windows (not built or run yet)

In an MSYS2 MINGW64 shell (MinGW-w64 GCC; MSVC is unchecked, see Platforms):

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-pkgconf mingw-w64-x86_64-SDL2 mingw-w64-x86_64-python
tools/build_msys2.sh --test
```

It builds `nanpa2-port.exe` (a GUI program, so no console; set `NANPA2_LOG_FILE` for the
log), named so it never replaces the game's own `nanpa2.exe`, and copies it with `SDL2.dll`
and the MinGW runtime DLLs into `build/dist`.

### By hand

```sh
cmake -S . -B build -G Ninja   # -DNANPA2_GAME_DIR=game for the tests
ninja -C build
```

## Architecture

```
src/game/      the game: script VM, opcodes, expressions, text, animation, town map, palette and
               fades, picture loading, start-up. It works on one state object G (state.h), the
               original's data area in its own layout, and calls only the platform API below.
src/platform/  the platform API and its host implementation (SDL2): files.h (the game's DOS paths on
               the host, FLAG files in the save folder), surface.h (8-bit surfaces by handle, blits,
               palette, the screen), text.h (Big5 glyphs), textlayer.h (hi-res text over the
               presented screen), window.h (WM_* messages, the 16 ms timer,
               the cursor), sound.h (music, voices), logo, clock (pacing), present (Metal or SDL_Renderer)
src/dev/       dev tools: record and replay (state checkpoints), the exploration bot, scripted input,
               the live status file
src/           trap.c, lzh.c and gp4.c (GP4/ANI pictures), fsys.c, launch.c, main.c
```

The refactor that led here (the original code run in an interpreter, then reimplemented in C on an
emulated PC, then freed of it step by step) is in `docs/refactor.md`, stage by stage, with the
architecture summary and how the save files stay byte-compatible.

## Game data

Copy the CD contents into `game/` and extract `FLAG0`..`FLAG6` from `data1.cab` there
(`unshield x data1.cab`). See `game/README.md`.

Neither `nanpa2.exe` nor any file made from it is needed: the engine reimplements the game code in C and
defines the start values of its game state in code (`src/game/state.c`). (Earlier versions cut a
`nanpa2.dat` from the exe at the first launch; a `nanpa2.dat` or `nanpa2.exe` left in the folder is
ignored. The ctest `standalone` runs the game with neither.)

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
`game/`); `./game`. If none of them has `DATA/`, a folder picker opens
(macOS) and the choice is remembered in `game-folder.txt` in the save folder
(`~/Library/Application Support/nanpa2/nanpa2/` on macOS, next to the `flag*` save files;
`NANPA2_SAVE_DIR` moves both). A folder that is not the game shows a message box.

### Display

F12 (on iOS a three-finger tap) cycles four modes: **CRT HD** (default), **CRT 480**, **Sharp**,
**Smooth**; the name shows in the window title for two seconds. `--display hd|480|sharp|smooth`
(or `NANPA2_DISPLAY`) picks one; `--no-crt` is `--display sharp`. A plain launch remembers the last
mode in `display.txt` next to `game-folder.txt` (scripted, headless, shot and replay runs neither read
nor write it; `NANPA2_DISPLAY_PREF=1` forces it).

- CRT HD: a late-90s flat Sony FD Trinitron (GDM-FW900 / F520 class) showing a ~full-HD desktop mode with
  the 640x480 scaled up by the video card. The picture is resampled in linear light onto a virtual
  raster of `--crt-lines N` lines at 4:3 (600..1600, default 1080 = 1440x1080;
  `NANPA2_CRT_LINES`) with an area-averaged scaler (equal pixel widths, a one-pixel soft seam at
  most; `--crt-scaler smooth` uses Catmull-Rom instead, `NANPA2_CRT_SCALER`). The beam, scanlines
  and aperture grille work at the raster's line pitch, so the scanlines are about three times finer
  than CRT 480's; they and the grille fade in as the screen gets larger (clean upscale at 1080p, fine
  lines at 4K or on an iPad). Glow, damper wires, vignette and dark glass are the tube's.
- CRT 480: the tube showing 640x480, one scanline per game line (the earlier CRT, unchanged).
- Sharp: no CRT, area-averaged scaling in linear light: every pixel the same width at any scale,
  exactly the plain pixels at integer scales.
- Smooth: no CRT, Catmull-Rom bicubic in linear light.

The SDL_Renderer presenter (non-Apple) approximates CRT HD with scanlines at the virtual raster's pitch
and Sharp/Smooth with an integer-scaled copy scaled linearly.

#### Text layer

The game draws its text as the 2000 original did, one 1-bit glyph at a time into the 640x480 screen,
and at a window of 1280x960 or more those one-pixel strokes are smeared. The text layer draws the same
characters as antialiased vector text at the display's real resolution, over the scaled picture. It is
on by default (**F11** toggles it; on iOS there is no gesture, set it with `--text` or the preference
file): `--text hires|bitmap` (or `NANPA2_TEXT_LAYER`), remembered with the display mode on a second
line of `display.txt` (`text hires`; the same rule: plain launches only). Not to be confused with the
debugging variable `NANPA2_TEXT=threshold|outline`, which changes how the bitmap glyphs are made.

- It never touches the game: the screen surface, the game state, the saves and the replay hashes are
  unchanged (`--text bitmap` presents byte-identical frames). The platform records each cell the game
  drew (character, font, size, colours, the ink mask); at each frame the presenter checks the cell
  against the screen, and shows the vector glyph only where the pixels still are exactly what the glyph
  drew. A cell that was drawn over, erased, scrolled or wiped fails the check and the picture shows
  through. Palette changes change colours, not indices, so the text fades with the picture. The one
  exception to an exact match: when every differing pixel of the frame lies in one 32x32 box (the
  game's mouse pointer is a 32x32 sprite drawn into the screen) and the cell keeps most of its ink, the
  cells touching it are shown with those pixels repainted over the vector glyph as they are.
- Glyphs come from the same font as the bitmap ones (the strokes widened a little to match their
  weight, fallback face for kana), rasterised at the on-screen size into a glyph atlas, and composited
  in linear light as opaque cells: CRT HD draws them into the virtual raster before the tube model (so
  they get the same beam, grille and glow), Sharp and Smooth draw them on the output. CRT 480 always
  shows the game's own glyphs (it is the period-correct mode). The text drawn into the picture files
  themselves (the name-entry grid) is not text the game draws and stays as it is.
- The Metal presenter implements it; the SDL_Renderer presenter accepts the option and the key and shows the
  game's glyphs.
- `NANPA2_LOG_TEXT=1` (with a `--crt-shot` or `NANPA2_SHOT_PRESENT` capture) prints what the layer did,
  `=2` also how well the vector glyphs line up with the bitmap ones; the status file has a `text` object
  with the counts and timings.

### Game speed

The game runs at the speed it had on the 486-class PC of the Taiwan release: its animation, fades
and text delays follow the 16 ms timer in real time, and its busy work (script expressions, GDI text,
picture decoding, the dissolve wipe) takes the time it took there (`src/game/pace.h`, derived in
docs/refactor.md "R3"; a dissolve is 4.1 s, 1.1 s with a button held). `--speed X` (or
`NANPA2_SPEED=X`) scales all of it: 1.0 is the original's, 2.0 twice as fast, 0 as fast as the host can
(the game clock is then the model's virtual time, as in deterministic mode; for tests and long scripted
runs). `NANPA2_TURBO=N` is the old name of `NANPA2_SPEED=N`; `--cpu-mips` is gone. `NANPA2_TIMELOG=1`
prints a line per fade, wipe, text character, player step and file open with the game time in ms.

### Recording and replay (development)

```sh
./build/nanpa2 --game game --record play.rec        # play (or script) and record
./build/nanpa2 --game game --replay play.rec        # replay, compare every checkpoint
tools/replay_diff.py a.rec b.rec                    # compare two recordings
tools/replay_diff.py --sequence a.rec b.rec          # by content, for runs with different pacing
```

`--record FILE` (`NANPA2_RECORD`) and `--replay FILE` (`NANPA2_REPLAY`) switch on the
deterministic mode (`--deterministic`, `NANPA2_DETERMINISTIC=1`): the game clock follows the
virtual time of the pacing model (below) instead of the host's, so two runs of the same input are identical. A
recording holds the input messages by game tick and a hash of the game state every
`NANPA2_CP_TICKS` ticks (250); a replay needs the save folder as it was at the start and stops
at the first checkpoint that differs (exit code 3, naming the key). `NANPA2_REPLAY_CHECK=li`
compares only the layout independent keys. Recordings hold derived game state: keep them out of
the repository. Details: docs/refactor.md.

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
current Homebrew bottles that is macOS 26). It contains no game data and nothing of `nanpa2.exe`
(ctest `no_game_bytes` scans it). On
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
active flag 0x4ac4ec, timer, game speed), the logo movie, music and voice. A
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
  of killed instances are not removed. Unchecked: MSVC (the sources use GNU extensions and POSIX `getline`, and CMake passes GCC/Clang
  flags, so MinGW-w64 or clang-cl is the safer bet), non-ASCII paths (the C
  runtime's `fopen` takes the ANSI code page) and no folder picker. The target is a
  `WIN32_EXECUTABLE` named `nanpa2-port.exe` (SDL2main supplies WinMain), so no console
  opens; the log goes to `NANPA2_LOG_FILE`.
- Both: the presenter is `src/platform/present_sdl.c` (SDL_Renderer); the status file works,
  the smoke test's desktop checks are macOS only.
