# CLAUDE.md

Guidance for Claude Code in this repository.

## What this is

A native port of 同級生２ (Doukyuusei 2), Taiwan Traditional Chinese release (`nanpa2.exe`,
built 2000-07-10), to SDL2. The original engine is reimplemented in C from static analysis of
`nanpa2.exe`; the game's own data files are read unchanged. macOS presents through a
CAMetalLayer; other platforms use SDL_Renderer. `README.md` is the overview and build guide,
`NOTES.md` the dated status log, `re-notes.md` the reverse-engineered formats and native
addresses.

## Hard rules

- **Game data never leaves this machine.** `game/` (the CD contents plus `nanpa2.exe` and
  `FLAG0..6` from `data1.cab`), `*.iso` and `decomp/` (Ghidra project and decompiled C) are
  gitignored. Never commit or upload them, and never copy tables or text out of them into the
  source beyond small format constants (code tables, offsets) needed to read the data.
- Never run any `.exe`. Static analysis of `nanpa2.exe` (Ghidra headless, objdump) only.
- Commit as `pyang2045 <39607593+pyang2045@users.noreply.github.com>` with `TZ=UTC git commit`.
  No personal information (home paths, real names, emails) in commits, docs or release files.
  Stage only the files you mean to commit.
- After each fix or milestone, add a dated `NOTES.md` entry citing commit hashes and native
  addresses; record format findings in `re-notes.md`.
- Keep the platform split: game code never includes Metal or Objective-C; everything
  platform-specific goes behind `src/platform/present.h` (and later audio/input headers).
- Ask before outward or destructive actions (pushing, creating remotes, deleting files).

## Layout

The game logic is C (`src/game/`, ordinary calls and typed state: `docs/refactor.md`, R2). The
only thing of the original machine it still touches is the emulated address space (`src/x86/`) that
its state `G` (`src/game/state.h`, the whole data area 0x431000-0x4c0000 in native layout) is aliased
into, and the thunk layer below: Win32, DirectDraw, COM and C runtime calls are made by native
address through `hle_invoke` (`src/game/ctl.h`), which pushes the arguments on the emulated stack and
calls `hle_call`. No game instruction runs: nothing decodes `nanpa2.exe` code. The game needs only
the exe's built-in data (tables, strings, import directory, globals' start values), which
`src/x86/asset.c` cuts once from the player's `nanpa2.exe` into `nanpa2.dat` (first launch or
`nanpa2 --make-assets`; local, gitignored, never shipped; no code bytes). The game then runs
without the exe (`--exe` and `NANPA2_INTERP=1` still load the whole exe; `src/assets.c` decides).
The build needs neither the exe nor capstone and the binary holds nothing of the game (ctest
`no_game_bytes`, `asset_only`). The recomp backend, the interpreter, lockstep and the differential
harness were removed in R2.

- `src/x86/`: the emulated address space (memory, aliases onto `G`, heap, stack), the CPU registers
  the HLE layer reads its arguments from, nothing else (pacing is `src/hle/clock.c` and `src/game/pace.h`),
  `asset.c` (`nanpa2.dat` writer and loader; `src/assets.c` picks asset or exe).
- `src/hle/`: `hle.c` (import/COM thunks, HLE overrides by native address), `win32.c` (message queue,
  SDL events to WM_*, timer, the pump), `ddraw.c` (surfaces, Blt, palette), `text.c` (GDI text via
  stb_truetype, Big5), `sound.c` (SDL audio, MP3 via minimp3), `crt.c` (MSVC CRT by address),
  `dev.c` (scripted input and screenshots), `replay.c` (record and replay).
- `src/game/`: the game: `ctl.h` (cost, poll, pump, quit, the HLE boundary), `loop.c`/`ops*.c`/`vm.c`
  (the script VM: `vm_pc`, nested `vm_loop()`, handlers in opcode tables), `expr.c`, `text.c`,
  `screen.c`/`wipe.c`/`anim.c` (graphics), `file.c`, `startup.c` (`game_main`, window procedure),
  `map*.c` (town map), `state.h` (G).
- `src/`: `lzh.c` (GP4/ANI decompressor), `gp4.c`, `fsys.c` (case-insensitive files), `main.c`,
  `launch.c` (game folder lookup and picker, remembered path, log file for Finder launches),
  `platform/present_metal.m` (macOS, CRT shader), `platform/present_sdl.c`,
  `platform/dialog.h` (`dialog_macos.m` folder picker, `dialog_none.c` elsewhere).
- `tools/`: `disasm.py` (table-aware objdump to `decomp/game.asm`), `peek.py`,
  `lzh.py` + `gp4png.py`, `sheet.py` (contact sheet of BMP shots), `bundle_macos.sh`
  (target `nanpa2_app`: `build/同級生２.app` with SDL inside).
- `ghidra_scripts/DecompileAll.java`; `tests/decode_all.c`; `third_party/` (stb_truetype,
  minimp3, public domain).

Refactor (docs/refactor.md): replay is the check for every step (`--record FILE`, `--replay FILE`,
`NANPA2_DETERMINISTIC=1`); the game state is the struct `G` of `src/game/state.h` (R1), the
logic runs as ordinary C calls (R2), pacing (R3) and the platform calls (R4) are next.

Dev hooks: `NANPA2_IMAGE_TRACE=FILE` (which bytes of the PE image a run touched, written at exit;
`docs/reimpl.md` "The asset"), `NANPA2_LOG=1` (imports, fopen, missing glyphs), `NANPA2_SHOT_DIR` +
`NANPA2_SHOT_SCHEDULE=start,interval,count`, `NANPA2_INPUT="click,MS,X,Y;key,MS,VK;quit,MS"`
(screen coordinates). `--view NAME --crt-shot OUT.bmp` captures presenter output.
`NANPA2_GAME_DIR` (game folder without `--game`), `NANPA2_LOG_FILE`; `NANPA2_SAVE_DIR` also
holds `game-folder.txt` and `nanpa2.log`, so app tests leave the real save folder alone.

## Build and run

```sh
cmake -S . -B build -G Ninja && ninja -C build
./build/nanpa2 --game game                       # play (F12 toggles CRT, --no-crt)
./build/nanpa2 --view                            # picture viewer: Left/Right, Esc
./build/nanpa2 --view A_END2.GP4 --shot out.bmp  # headless: write the screen and exit
./build/decode_all game                          # expect "ok 705 bad 0"
ctest --test-dir build -R smoke --output-on-failure  # after each merge: one window, renders
                                                 # (Skipped = passed, screen not visible)
cat $TMPDIR/nanpa2-status-*.json                 # live state of every running instance
```

A black or frozen window: read the status file first (`warning`, `mode`, `present`,
`game`); see README "Diagnosing a running instance". Scripted test runs use
`NANPA2_HEADLESS=1` and must not show a window.

Tests while the screen is locked: use headless. With the screen locked a windowed launch
can hang in startup (SDL_Init / CAMetalLayer setDevice in IOServiceOpen, process state U,
unkillable until unlock; the status file names the `stage`). `NANPA2_HEADLESS=1` uses SDL's
dummy video driver and no GPU, so it always runs; the smoke test checks the lock first.

The log line `Renderer: Metal | <GPU>` confirms Metal. Wrap scripted runs:
`perl -e 'alarm 10; exec @ARGV' ./build/nanpa2 ...`.

## Reverse engineering

Ghidra 12.2 and its JDK live in `~/git/porting/tools/`:

```sh
G=~/git/porting/tools/ghidra_12.2_PUBLIC
export JAVA_HOME=~/git/porting/tools/jdk-25.0.5+8/Contents/Home
$G/support/analyzeHeadless decomp/ghidra Nanpa2 -import game/nanpa2.exe -overwrite \
  -scriptPath ghidra_scripts -postScript DecompileAll.java decomp
```

About a quarter of the game functions are hand-written assembly with register arguments
(`unaff_ESI`, `in_EAX` in the decompile); read those with
`objdump -d --x86-asm-syntax=intel --start-address=0x... --stop-address=0x... game/nanpa2.exe`.
Game code is 0x401000-0x4077ac (142 functions, `docs/reimpl.md`); a statically linked MP3
decoder library follows to 0x412c81, then the MSVC runtime from 0x412c90. Cite native addresses in code
comments, commits and notes.
