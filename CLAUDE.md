# CLAUDE.md

Guidance for Claude Code in this repository.

## What this is

A native port of 同級生２ (Doukyuusei 2), Taiwan Traditional Chinese release (`nanpa2.exe`,
built 2000-07-10), to SDL3. The original engine is reimplemented in C from static analysis of
`nanpa2.exe`; the game's own data files are read unchanged. macOS presents through a
CAMetalLayer; other platforms use SDL_Renderer. `README.md` is the overview and build guide,
`NOTES.md` the dated status log, `re-notes.md` the reverse-engineered formats and native
addresses.

## Hard rules

- **Game data is never committed, uploaded or published.** `game/` (the CD contents plus `nanpa2.exe`;
  `FLAG0..6` from a copied install are optional: the engine generates blank saves), `*.iso` and `decomp/` (Ghidra project and decompiled C) are
  gitignored. Never commit, push, upload or publish them (no repo, release, artifact, issue or CI cache), and never copy
  tables or text out of them into the source beyond small format constants (code tables, offsets) needed to read the data.
  The one allowed copy is the user's own files onto the user's own devices, when the user asks (for example
  `NANPA2.iso` into the iPad app's Documents folder, or the CD folder into the simulator container).
- Never run any `.exe`. Static analysis of `nanpa2.exe` (Ghidra headless, objdump) only.
- Commit as `pyang2045 <39607593+pyang2045@users.noreply.github.com>` with `TZ=UTC git commit`.
  No personal information (home paths, real names, emails) in commits, docs or release files.
  Stage only the files you mean to commit.
- After each fix or milestone, add a dated `NOTES.md` entry citing commit hashes and native
  addresses; record format findings in `re-notes.md`.
- Keep the platform split: game code never includes SDL, Metal or Objective-C; everything
  platform-specific goes behind the platform API headers in `src/platform/` (`files.h`, `surface.h`,
  `text.h`, `window.h`, `sound.h`, `present.h`, `dialog.h`). A new host service gets a header there; the
  game calls it directly.
- Ask before outward or destructive actions (pushing, creating remotes, deleting files).

## Layout

The engine is plain C (refactor stages R0-R4, `docs/refactor.md`): no x86 code runs and nothing emulates
the PC the game was written for. The game logic calls a small platform API; the platform implements it on
SDL3. Game code never includes SDL, Metal or Objective-C.

- `src/game/`: the game. `state.h` defines `G`, one object that is the exe's data area (0x431000-0x4c0000) in
  its native layout, initialised by code (`state_init`, `state_defaults`: no data file is read) and the only game state; `gmem.h` has
  `rd8..wr32`/`g_ptr` over it by the original's numbering ("G addresses": segment bases, animation
  pc, labels are integers that index G, never host pointers; outside G traps). The VM (`loop.c`, `ops*.c`,
  `vm.c`, `expr.c`), text, animation, the town map (`map*.c`), palette and picture/rectangle/cursor
  routines (`screen.c`, `wipe.c`), `startup.c` (`game_main`, window procedure), `timer.c` (the 16 ms tick),
  `file.c` (the game's file loader), `ctl.c` (the message pump as a call, `ctl_quit` unwinding, the clock
  charge `ctl_work_us`), `pace.h` (the time model), `state_view.c` (the FLAG serializer, the checkpoint
  views, the data hash). Native addresses stay in comments as documentation.
- `src/platform/`: the platform API and its host implementation: `files.h` (the game's DOS paths on the
  host, FLAG files in the save folder), `surface.h` (8-bit surfaces by handle, pixel pointers, BltFast,
  fill, palette, the screen), `text.h` (Big5 glyphs, stb_truetype), `window.h` (WM_* message queue, the
  16 ms timer, cursor), `sound.h` (SDL audio, MP3 via minimp3), `logo.c`, `clock.[ch]` (pacing, `--speed`),
  `present.h` (macOS: `present_metal.m` CAMetalLayer CRT shader; else `present_sdl.c` SDL_Renderer),
  `dialog.h` (`dialog_macos.m` folder picker, `dialog_sdl.c` (SDL_ShowOpenFolderDialog) elsewhere).
- `src/dev/`: `replay.c` (deterministic mode, `--record`/`--replay`, state checkpoints), `explore.c` (the
  exploration bot), `dev.c` (scripted input, screenshots, `NANPA2_SAVETEST`), `status.c` (live status file).
- `src/`: `hooks.[ch]` (the dev hook table: the only way `platform/` and `trap.c` reach `dev/`; `dev_install_hooks`
  in `dev/dev.c` fills it from `main`; nothing outside `dev/` and `main.c` includes a `dev/` header, test `headers`),
  `hash.[ch]` (the checkpoint hash), `trap.c` (`game_trap`, probes), `lzh.c` + `gp4.c` (GP4/ANI), `fsys.c` (case-insensitive files),
  `launch.c` (game folder lookup, remembered path, log file for Finder launches), `main.c`.
- `ios/`: the iOS build, a standard Xcode project (`nanpa2.xcodeproj`, `Info.plist`, `Assets.xcassets`). CMake does not build iOS;
  `tools/gen_xcodeproj.py` is the source of the pbxproj (edit it, rerun, commit; test `xcodeproj` checks it against
  CMake's source list), `tools/fetch_sdl.sh` builds the gitignored `ios/third_party/SDL3.xcframework`, and
  `tools/build_ios.sh` wraps `xcodebuild` (simulator, `--device`, unsigned `--ipa`). No team IDs or paths in the project.
- `tools/`: `build.sh`, `build_msys2.sh`, `bundle_macos.sh` (target `nanpa2_app`: `build/同級生２.app` with
  SDL inside), `replay_diff.py`, `explore.py`, `gp4png.py`, `lzh.py`, `sheet.py`, `peek.py`, `disasm.py`
  (table-aware objdump to `decomp/game.asm`), `timing_stats.py`; `ghidra_scripts/DecompileAll.java`;
  `tests/` (ctest: headers, selftest, replay_det, wavcheck, no_game_bytes, standalone, smoke, decode_all);
  `third_party/` (stb_truetype, minimp3, public domain).

The save files (`FLAGn`, 0xc00 bytes) are `G.save` in the original's byte layout, written and read only
through `state_save_export/import` (`state_view.c`); `docs/refactor.md` lists the fields. The engine assumes a
64-bit little-endian host.

Start save (`src/dev/startsave.h`, README "Start save"): a plain interactive launch (the `use_pref` condition of
`display_setup`, and no EXPLORE/SAVETEST) with no FLAG files in the save folder and the game folder's FLAGs absent or
all identical plays the Prologue offline (`platform/offline.h`) and writes `flag6` + `flag0`; `--make-start-save`
does it headless and exits; `--no-start-save` / `NANPA2_NO_START_SAVE=1` opt out (`tests/smoke.py` `clean_env` sets
it); `startsave-off` in the save folder turns it off; `NANPA2_START_SAVE_SEED/_FRAMES/_LOG/_SHOTS=dir,every/_NOFF`
are dev knobs. iOS: `dialog_saves_popup` (`NANPA2_RESET_POPUP`).

Dev hooks: `--text bitmap` / `NANPA2_TEXT_LAYER=bitmap` (developer/test only: the game's own glyphs without the hi-res text layer; the byte-identity tests use it; hi-res is always on otherwise and not a user option; `NANPA2_LOG_TEXT=1` logs the text scale), `NANPA2_IMAGE_TRACE=FILE` (which bytes of the PE image a run touched, written at exit;
`docs/reimpl.md` "The asset"), `NANPA2_LOG=1` (imports, fopen, missing glyphs), `NANPA2_SHOT_DIR` +
`NANPA2_SHOT_SCHEDULE=start,interval,count`, `NANPA2_INPUT="click,MS,X,Y;key,MS,VK;quit,MS"`
(screen coordinates). `--view NAME --crt-shot OUT.bmp` captures presenter output.
`NANPA2_GAME_DIR` (game folder without `--game`), `NANPA2_LOG_FILE`, `NANPA2_SPEED`/`--speed`, `NANPA2_DETERMINISTIC`, `--record`/`--replay`
(`NANPA2_REPLAY_CHECK=li,raw.clock,raw.gdata` picks the checkpoint keys), `NANPA2_EXPLORE=SEED`
`NANPA2_EXPLORE_FRAMES=N` (the bot); `NANPA2_SAVE_DIR` also
holds `game-folder.txt` and `nanpa2.log`, so app tests leave the real save folder alone.

## Build and run

```sh
cmake -S . -B build -G Ninja && ninja -C build
./build/nanpa2 --game game                       # play (F12 toggles CRT, --no-crt)
./build/nanpa2 --view                            # picture viewer: Left/Right, Esc
./build/nanpa2 --view A_END2.GP4 --shot out.bmp  # headless: write the screen and exit
./build/decode_all game                          # expect "ok 705 bad 0"
ctest --test-dir build --output-on-failure       # all: selftest, replay_det, wavcheck, no_game_bytes,
                                                 # standalone, and smoke (one window, renders;
                                                 # Skipped = passed, screen not visible)
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

Ghidra 12.2 and a JDK are installed outside the repo (the paths below are the maintainer's layout; adjust):

```sh
G=~/git/porting/tools/ghidra_12.2_PUBLIC
export JAVA_HOME=~/git/porting/tools/jdk-25.0.5+8/Contents/Home
$G/support/analyzeHeadless decomp/ghidra Nanpa2 -import game/nanpa2.exe -overwrite \
  -scriptPath ghidra_scripts -postScript DecompileAll.java decomp
```

About a quarter of the game functions are hand-written assembly with register arguments
(`unaff_ESI`, `in_EAX` in the decompile); read those with
`objdump -d --x86-asm-syntax=intel --start-address=0x... --stop-address=0x... game/nanpa2.exe`.
Game code is 0x401000-0x4077ac (142 functions, `docs/reimpl.md`, historical: the C engine replaced all of
it); a statically linked MP3 decoder library follows to 0x412c81, then the MSVC runtime from 0x412c90.
Cite native addresses in code comments, commits and notes.
