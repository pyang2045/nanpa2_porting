# 同級生２ native port

A from-scratch reimplementation of the engine of 同級生２ (Doukyuusei 2), Taiwan Traditional
Chinese Windows release (`nanpa2.exe`, 2000), on SDL2, so it runs natively on macOS (Metal),
Linux and Windows. It needs your own copy of the game CD; no game data is in this repository.

## Status

Early. Pictures (`.GP4`) decode and display; the script engine, text, sound and input are
not done yet. See `NOTES.md`.

## Build

Needs CMake, a C11 compiler and SDL2 (`brew install sdl2 cmake ninja`).

```sh
cmake -S . -B build -G Ninja
ninja -C build
```

## Game data

Copy the CD contents into `game/` and extract `nanpa2.exe` and `FLAG0`..`FLAG6` from
`data1.cab` there (`unshield x data1.cab`). See `game/README.md`.

## Run

```sh
./build/nanpa2 --game game        # picture viewer for now: Left/Right, Esc
```

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
occlusion), the presenter (frames given and presented, presents skipped as occluded / no
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
