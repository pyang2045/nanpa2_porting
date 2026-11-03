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
