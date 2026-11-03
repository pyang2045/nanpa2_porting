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

It holds the pid and start time, the mode (headless, scripted, every `NANPA2_*` variable
set), the window (SDL and on-screen window counts, flags, size, drawable size, focus,
occlusion), the presenter (frames given and presented, presents skipped as occluded / no
drawable / size 0, worst `nextDrawable` wait, mean luminance of the last frame, how long the
screen has been black), the game (frame counter 0x446292 and how long it has stood still,
active flag 0x4ac4ec, timer, last poll pc, CPU MIPS), the logo movie, music and voice. A
watchdog logs one `WARNING:` line, and fills `warning`, when the screen stays black or
nothing reaches the window for 8 s, the frame counter stops while the window is visible, the
main thread stalls, or a headless run shows a window; it names the likely cause (e.g.
`presents skipped, occluded=…`, `timer stopped: 0x4ac4ec=0`, `stuck polling at pc …`).

`NANPA2_HEADLESS=1` (scripted test runs) creates the window hidden: nothing appears on the
desktop.

### Smoke test

```sh
ctest --test-dir build -R smoke --output-on-failure   # or: tests/smoke.py build/nanpa2 game
```

Runs a plain interactive launch (real window and presenter, no `NANPA2_*` variables) for
35 s and checks from the status file: exactly one window, frames presented, a non-black
frame by 20 s, the frame counter advancing, no watchdog warnings; then a 6 s
`NANPA2_HEADLESS=1` run must show no window. It opens a window on this desktop; on failure
it prints the diagnosis and the last status. Set the game folder with
`-DNANPA2_GAME_DIR=...`.
