# NOTES

## 2026-11-03

- Disc identified: Taiwan Traditional Chinese 同級生２, `nanpa2.exe` linked 2000-07-10. Plain
  Win32 (DirectDraw 640x480x8, DirectSound, MCI for MP3/AVI, GDI text in 細明體 Big5).
- Headless Ghidra decompile: 342 functions, game code 0x401000-0x412210 (143 functions, 35 of
  them hand-written assembly with register arguments).
- GP4/ANI codec reversed (0x4075f0, LZHUF variant, see `re-notes.md`) and ported to C. All 705
  GP4/ANI files decode, and every GP4 matches its header size (`tests/decode_all.c`).
- SDL2 skeleton with a CAMetalLayer presenter on macOS (palette lookup in the fragment
  shader) and SDL_Renderer elsewhere; `--view`/`--shot` picture viewer.

Next: the MES script VM (start.mes, 0x4060d4 and the interpreter it calls), text drawing,
input, then ANI/A6/maps and sound.

## 2026-11-03 (static recompile)

- Chose static recompilation: `tools/recomp.py` lifts 0x401000-0x407800 (about 6400
  instructions, 1739 labels) into one dispatch function; jump tables at 0x40620b/0x406262
  (script VM) and others are read as data. Backward jumps poll the timer and presenter.
- HLE layer: Win32 imports, DirectDraw COM (Blt, BltFast, Lock, GetDC, palettes), GDI text
  (stb_truetype + Big5), MSVC CRT by address (sprintf 0x412c90, fopen 0x412d13, _strnicmp
  0x41a740 ...), sound overrides (music 0x402cb0, voice 0x403360), decompressor 0x4075f0,
  timer callback 0x4028c0.
- Fixes: timer needs 0x4ac4ec (WM_ACTIVATEAPP, posted from ShowWindow); 0x41a740 is
  case-insensitive (FLAG files); scripted clicks are held 120 ms so the game sees them.
- Font face chosen by coverage of Traditional-only glyphs (name match fell back to the
  Simplified face of Songti.ttc and dropped 時 張 麼 聲 噹 and the hero's name).
- CRT simulation in the Metal shader (curvature, scanlines, aperture grille, vignette; mouse
  mapping follows the curvature), scanline overlay in the SDL_Renderer path; F12 toggles.
- Boots through the CD check, logo, name entry and Prologue into the first dialogue.

## 2026-11-03 (sound)

- Sound path reversed (0x402ad0-0x4034a0, see `re-notes.md`): two DirectSound buffers only,
  music (MP3 streamed and looped through 0x4034a0/0x4031d0, or a WAV played once by 0x402cb0)
  and voice (0x403360, once; cut by clicks, Return/Space/Esc key-up and deactivation in the
  WndProc). No volume, pan or settings; MCI is only for the logo.
- `src/hle/sound.c` follows it: MP3 loops, WAV music and voices play once, a new piece or
  voice replaces the old, pause/resume on WM_ACTIVATEAPP (0x403220/0x403250; a finished WAV
  piece replays on reactivation; inferred from DirectSound rewinding a finished one-shot
  buffer, not verified in the exe).
  Linear resampling; 0x402c70 closes the device. 0x402f10/0x402f60/0x4031d0 (only reached
  from replaced functions) are handled as stubs instead of trapping.
- `NANPA2_AUDIO_DUMP=out.wav` writes the mixed output (44100 Hz S16 stereo, header kept
  valid every second). With no audio device the game runs silently; with a dump requested
  it falls back to SDL's dummy driver, which runs the mixer in real time. `NANPA2_LOG`
  logs each music/voice start and stop with its time and dump position.
- Checked with dumps: logo jingle `MUSIC\50.WAV` (1.4 s) and Prologue `42.WAV` (16.5 s) play
  once at their logged times, then silence; `38.MP3` starts at its log line; `54.MP3`
  (2.9 s) loops; pause/resume works. The scripted run never calls the voice opcode: the
  Prologue scripts (OPEN*.MES) have no voices, the first are in 02.MES (`\j\j10022.wav`
  style names). The voice path was checked with a small harness around `sound.c`.
- The scripted run is not deterministic now: the logo/name-entry phase took 7-19 s across
  runs (same binary), so the fixed click times often miss the name entry.

## 2026-11-03 (display)

- CRT reworked twice: first scale-aware (0f98b05), then, by request, as a flat Sony FD
  Trinitron (ad60954): no curvature (linear mouse mapping), vertical RGB aperture-grille
  stripes (three output pixels per triad; none at 1x, faint at 2x, full from 3.5x), damper
  wires at 1/3 and 2/3 height, fine Gaussian scanlines widening on bright pixels, light glow,
  mild vignette, dark grey glass. Passes: palette to linear light, separable blur (glow),
  CRT. Same look in the window and fullscreen, any size; F12 / --no-crt as before.
- `--crt-size WxH` (0x0 = drawable) and `--view-bmp SHOT.bmp` for checking the presenter
  with NANPA2_SHOT_DIR screenshots.
- Fullscreen: Alt+Enter, Cmd+F, `--fullscreen` (a485df0); hotkeys shared by the game and
  viewer loops through present_hotkey(); SetCursorPos maps through present_screen_to_window.
- SDL_Renderer fallback approximates the Trinitron with render targets (scanlines, stripe
  texture, damper wires, glow, vignette); `-DNANPA2_SDL_RENDERER=ON` builds it on macOS.
- LOGO.AVI plays (2b0d6c1): Cinepak decoder + AVI parser replace the MCI path of 0x401e20,
  frames go through present_frame_rgb (the native movie ran in 640x480x16), PCM on its own
  SDL audio device, skip on key/click as 0x401fd0. Scripted runs skip it.
- Text (0x401900, 80c7211): cells are 16/24/32 for em 15/24/32 (re-notes.md); the baseline now follows
  the ideographic ascender and is raised so Songti ideographs keep their bottom row in the
  32-px cell; ASCII is narrowed to the 8-px half-width cell. No shadow exists in the native
  text path. `--window WxH` sets the initial window size.
- Checked windowed (640x480-point window, Retina drawable 1280x960; 900x560 window,
  drawable 1800x1120, letterboxed) and fullscreen (3584x2240) with `--crt-size 0x0`, and
  640x480 / 1000x750 / 1920x1440 / 2560x1920 offscreen. The scripted run to the first
  dialogue stopped working at name entry this evening (the clicks on the name grid do not
  register) for the old baseline binary too, and with a fresh HOME (so not the shared save
  folder); likely the shared desktop (real mouse/focus). The text check used a harness that
  copies cells the way 0x401900 does.
- 31d5943: scanlines were invisible at exactly 2x (both output rows of a line sampled the
  beam symmetrically); the sampling is shifted half an output row around 2x. Grey scanline
  depth at 1280x960 3% -> 14%; black level (glass) is 18/19/21 of 255.

## 2026-11-03 (engine: speed, repeatable runs)

- Pacing survey (re-notes "Pacing"): every visible wait in the game code is on the 16 ms
  timer (frame counter 0x446292: 0x4075c6 frame waits, 0x403624 animation tick, 0x401a50
  wipe, 0x4028c0 fade, text delay = script variable 0x4452de). No delay loops, no
  timeGetTime. Survey covers startup to the Prologue; the map code 0x404132-0x405fb6 is
  not yet checked.
- Emulated CPU speed (2fe4f5a, f9b242d): recomp.py adds each block's instruction count
  to x86_icount; x86_throttle sleeps when it runs ahead of real time. `--cpu-mips N` /
  `NANPA2_CPU_MIPS`, default 25 (486DX2-66 at ~2.6 clocks/instruction), 0 = off.
  HLE work is charged too: decompressor 0x4075f0 70/input byte + 1.2/output byte
  (measured on the lifted native code), PeekMessage pump 500, blits 1/4 per pixel (1 keyed),
  TextOutA 5000 per glyph.
- Measured, Prologue run of 57 s, 25 MIPS vs unthrottled: palette fades 256 ms (16
  frames) both; dialogue lines (text speed 0) 0-5 ms both; 25.0 vs 2707 MIPS; user CPU
  2.7 s vs 55 s.
- Repeatable scripted runs (f9b242d): `NANPA2_INPUT_CLOCK=frames` schedules input and
  shots on the game's tick counter; scripted runs ignore the host mouse, keys and focus;
  `sync,0,FILE` events; `NANPA2_HEADLESS=1` skips the window. Three concurrent runs reach
  PRO.GP4 at the same frame. Causes of the earlier randomness: an occluded CAMetalLayer
  blocks `nextDrawable` up to 1 s (game and timer stall; a click's down and up arrive in
  the same pump), the real cursor/focus on a shared desktop, and wall-clock schedules.
  Input lists were also cut at 512 entries (~255 clicks), which looked like a hang.

## 2026-11-03 (black window)

- Interactive runs could show a black window: on focus loss collect_events posted
  WM_ACTIVATEAPP(0), whose handler clears 0x4ac4ec, so the 16 ms timer (0x4028c0) stops
  advancing the frame counter 0x446292 that the game waits on (e.g. poll at 0x4035c0/0x406f48)
  and music pauses (0x403220). Injected focus loss right after LOGO.AVI froze the counter at 2
  with a black screen for good; at 18 s it froze on the elf logo. Launched from a terminal the
  window was never activated (no SDL_WINDOW_INPUT_FOCUS), so a click on it and back to the
  terminal was enough.
- 31ffa68: focus loss no longer pauses the game (`--pause-inactive` /
  NANPA2_PAUSE_INACTIVE=1 restores 0x403220/0x4ac4ec behaviour); SDL_RaiseWindow at startup
  (the window now gets input focus); the Metal presenter skips frames while the window is
  covered (NSWindow occlusionState), since nextDrawable then blocks up to 1 s per call. That
  stall was reported from another run; hidden, minimized and second-window cases here did
  not reproduce it.

## 2026-11-03 (two black windows, self-diagnosis)

- "Two windows, both black" was two scripted runs of the engine agent, not the user's game:
  started with `NANPA2_HEADLESS=1 NANPA2_INPUT=... NANPA2_SHOT_DIR=...` from its worktree
  (one more seen here: pid 4608, started 23:22:10, `./build/nanpa2 --game ../../../game`,
  cwd `.claude/worktrees/agent-a2075...`). NANPA2_HEADLESS only skipped present_frame in
  ddraw_present; main.c still created and raised a visible window, which stayed black.
- 1d61bb2: headless runs create the window SDL_WINDOW_HIDDEN and do not raise it.
- 1d61bb2: live status file `$TMPDIR/nanpa2-status-<pid>.json` (src/hle/status.c), about once
  a second, removed on exit/SIGTERM/SIGALRM, stale ones removed at start: mode and NANPA2_*
  env, window and on-screen counts, occlusion, presenter counters (new present_stats(),
  luminance sampled at upload), frame counter 0x446292, 0x4ac4ec, last X86_POLL pc, logo,
  music, voice. Watchdog: one WARNING line with the likely cause after 8 s black or
  unpresented, 3 s frame counter standstill (timer on, not in the logo 0x401e20), 3 s main
  thread stall, or a headless run with a window on screen.
- tests/smoke.py (`ctest -R smoke`): plain launch 35 s plus headless 6 s. Passes on the Metal
  and SDL_Renderer presenters: one window, ~1480 frames presented, logo luma 14-179, then
  luma 20 and 60 after it (scenes not checked by eye), frame counter ~67/s, no warnings;
  headless: os_windows 0.
- The occlusionState skip (31ffa68) is correct: hiding the app 14 s mid-run gave
  `WARNING: no frame presented ... presents skipped, occluded=485`, the game kept running
  (frame counter advancing), and presenting resumed on unhide. Initial occlusionState is
  "covered" for a moment at startup and turns visible within the first second.

## 2026-11-03 (engine: through the Prologue scenes, in-game save and load)

- Scripted runs (frame clock, VM syncs from 734059c) now play OPEN.MES (classroom) -> S1
  (school gate) -> S2 -> S4 -> S5 (own room) -> S6 (living room, 8:00 PM) with no traps and
  no watchdog warnings. Menu-loop offsets used for syncs: OPEN @2a3c/@2a4f, S1 @26d5/@26e0,
  S2 @264f/@265a, S4 @2a05/@2a10, S5 @2760/@276b. S5 needs 考慮自己的事 -> 母親/美佐子/唯
  twice each (flags 13-18) before 移動 shows; 移動 -> 到唯的房間 then 客廳 loads S6.MES.
- In-game save: S5 存檔 -> 存檔 runs `04 21 31` (0x4070d5 -> 0x407188 -> 0x401160) for slot 0
  and slot 6: flag0 and flag6 (3072 bytes each) land in the SDL pref folder and the game
  prints 已經儲存。 Load: title "Data Load" -> diary picker -> the lit diary runs
  `04 21 30 03 02 53` (START1.MES 0x1342 -> 0x407152 -> 0x401050), reads flag0 and resumes
  S5.MES with the saved date, time and money. Round trip confirmed in-game.
- Pitfalls for input scripts: a click outside every menu item picks the highlighted item,
  so advance clicks must not overlap a menu; a move must land after the menu is drawn;
  menus return the item's index as written, with hidden items packed out of the 2x2 grid.
- The map/drawing code 0x404132-0x405fb6 has no frame waits (856a896).
- Throttled CPU over the 349 s S5->S6 run: 46.6 s user (13 %), against 4.7 % in the
  Prologue classroom; not yet broken down.

## 2026-11-04 (engine: into the main game, host CPU, right-click)

- Prologue to S7: S6's menu path (talk ×2, listen ×3, 移動 -> 回到自己房裡) loads S7.MES. The
  Prologue runs S1-S63 and S63 jumps to OPEN1.MES, the first main-game scene (12/22
  Sunday). To get there without 57 more scenes, a test save in a separate folder
  (`NANPA2_SAVE_DIR`, 5891002) names open1.mes at offset 0 of the save block; the title's
  Data Load then starts OPEN1. Clicking Yui's mouth (hotspot 0x0101) three times loads
  40.MES (own room, point-and-click); hotspots respond (drawer, desk, computer) with no traps.
- Not reached: the town map (TOWN.MES, MATI1.MAP). In 40.MES the right-click menu (到客廳去
  ...) stays off because 40.MES 0x274a copies a table into flags 1978-2007 and flag 2000
  becomes 15 (`if F[2000] == 0` gates the handler). The table is read through registers the
  synthetic save carries over from S5, so this is probably the test save rather than the
  engine; a save made by playing S7-S63 would settle it.
- Right-click (re-notes "Main game scenes"): main-game scenes open a script pop-up of
  self actions (OPEN1: 伸懶腰, verified; 40.MES: 到客廳去 / 待在原地 / 結束遊戲 / 儲存、讀取);
  Prologue menus only cancel.
- Host CPU of scripted runs (5891002, 7af68bd): dev_inject and the VM sync check scanned the
  whole input list on every pump and every script opcode; S5->S7 run 38.7 s -> 27.2 s user
  over 415 s. Scripted runs now pump Cocoa events every 16 ms: 16.6 s -> 14.9 s over 172 s.
  What is left is mostly the hidden window's event pump and the per-present frame copy
  (ddraw.c:85); the lifted game code itself is about 0.5 % of samples. Not the status thread.
- `NANPA2_HOTSPOTS=1` logs the hit-test rectangles (probe 0x4071bb).

## 2026-11-04 (smoke test whatever the desktop state, GPU-free headless)

- The smoke test failed whenever nobody could see the screen (locked, display asleep, window
  covered): presents are skipped then (correctly), so "frames presented" could not hold.
  Worse, with the screen locked (seen 2026-11-03 17:30 UTC, CGSSessionScreenIsLocked set,
  display awake) a windowed launch hangs in startup, even with NANPA2_HEADLESS: SDL_Init ->
  Cocoa_AddDisplay -> CVDisplayLinkCreateWithCGDisplay -> IOServiceOpen (Metal build: also
  CAMetalLayer setDevice), in process state U, so alarm/SIGKILL only land after the unlock.
  Not checked whether the agent shell's sandbox contributes.
- 1e3aaaf: the status file is written from before SDL_Init (`stage`: sdl_init, window,
  present_init, game_init, running; watchdog key `init` names a stuck stage); desktop state
  (src/platform/desktop_macos.c: CGSSessionScreenIsLocked, CGDisplayIsAsleep; NSApp.hidden);
  `warning_key` and the cumulative `warned` list. Metal offscreen probe: while presents are
  skipped for occlusion, the skipped frame is rendered offscreen (CRT off, so black stays
  black) once a second and its luminance recorded.
- 1e3aaaf: NANPA2_HEADLESS=1 now means SDL's dummy video driver and a software presenter path
  chosen at run time in both presenters (no window server, no GPU); ddraw_present hands
  headless frames to it, so luminance and NANPA2_SHOT_PRESENT captures (plain 640x480)
  work. Runs fine with the screen locked: logo ~16 s, then frame counter 0x446292 ~65/s.
- tests/smoke.py (1e3aaaf, fac2a65): reads the lock state first and skips the interactive run if locked or
  asleep; a covered window is checked through the probe; a 24 s headless run checks game
  progress and non-black frames; exit 77 (ctest SKIP_RETURN_CODE) = passed but on-screen
  presentation not verified; never blocks on an unkillable child, reports the hang stage.
- Checked with the screen locked: smoke Skipped ("SMOKE PASS (desktop screen locked ...)")
  on the Metal and SDL_Renderer builds; broken builds (black frames; timer 0x4028c0 never
  ticking) FAIL ("no non-black frame by T=20 s"; "frame counter does not advance" plus the
  `frame` warning at pc 00406f48). The visible and covered-window paths were checked only
  with synthetic status samples (the screen stayed locked); run the smoke test once with the
  desktop visible and once with the app hidden.

## 2026-11-04 (engine stage 4: the 40.MES right-click blocker, the town map)

- The 40.MES blocker (no right-click menu, flag 2000 = 15) came from the synthetic test save,
  not from the engine. The save was an S5 diary snapshot (flag 2010 = 1) renamed to OPEN1.MES.
  Real diary saves set flag 2010 only around the save itself. A title load stashes flags
  1978-2007 in script segment 3 (START1.MES 0x1290-0x1342), and the loaded scene's entry block
  copies them back (40.MES 0x274a). OPEN1.MES has no entry block and loads pictures into
  segment 3 first, so 40.MES restored picture bytes. See re-notes "Diary saves and the global
  flags". Every flag-2010 write in the game's scripts was checked: each set is followed by a
  clear in the same file.
- Empirical checks (hidden headless runs, scratch NANPA2_SAVE_DIR):
  - With flag 2010 cleared in the scratch save, 40.MES shows 到客廳去 and leads to 41.MES.
  - A game-made save works end to end: in 40.MES, click the bed, 睡1小時, 寫日記, book 1 →
    FLAG1 naming 40.mes, flag 2010 = 1. A fresh run then loads it from the title (Data Load,
    book 1). 40.MES runs its restore block (START1 stash → 0x274a), the right-click menu
    appears, and 到客廳去 leads to 41.MES and misako.mes.
  - The S7 → S63 Prologue was not replayed; blind clicking stays on S5's menu forever.
  - No engine bug was found, so there is no new regression check.
- f59ca24: NANPA2_TURBO=N runs the game timer (hle_timer_callback; frame counter 0x446292) and
  the CPU throttle N times faster. NANPA2_INPUT_CLOCK=frames schedules follow it, so long
  scripted routes take about a quarter of the time at N = 4. The hotspot log now shows up to
  100 rectangles (40.MES has more than 40).
- bb39197: `down,MS,X,Y` / `up,MS,X,Y` input events hold the left button.
- Town (TOWN.MES, ext op 0x26 → 0x403a70, subcommands through table 0x435576):
  - 41.MES right-click, 4th item → TOWN.MES, which loads mati1.gp4, chara1.gp4, mati1.map
    (128x72 16-bit cells after an 8-byte header) and mati1.c5.
  - The walk loop is TOWN.MES 0x22b3-0x2320. `04 26 3c` (0x4051c2) returns S = 0xffff while
    nothing happens. The player walks towards the mouse while the left button is held. A
    click on an object near the player (0x40500e, within about 0x30-0x50 px) returns its
    code. Standing on a trigger (0x404f8c) returns its code + 50. A click at (0xf8-0x187,
    0x120-0x13f) returns 0xfffe. Any other value breaks the loop, and the script dispatches
    on V = S.
  - Clicking the house just above the start position (screen (300,130)) enters 39.MES (in
    front of the house; first visit is an encounter scene). Its right-click menu offers
    回到家裡 / 進咖啡廳 / 待在原地 / 到其他地方.
- Timing: with NANPA2_INPUT_CLOCK=frames, inputs still land within a few frames of their
  target (game time comes from a host-paced timer). One house click out of three missed, so
  routes repeat such clicks until the next sync.

## 2026-11-04 (packaging: macOS app, game folder lookup, log file, portability)

- 015458c: `ninja -C build nanpa2_app` (or `-DNANPA2_APP_BUNDLE=ON`) builds
  `build/同級生２.app` with tools/bundle_macos.sh. SDL here is Homebrew's sdl2-compat
  (/usr/local/opt/sdl2-compat), which dlopens SDL3, first from `@loader_path/libSDL3.dylib`:
  both go into Contents/Frameworks (SDL3 under that name), load commands rewritten to
  @executable_path/@loader_path, ad-hoc signed; lsof on a running app shows only the
  bundled copies. LSMinimumSystemVersion comes from the binaries' minos: 26.0 with these
  bottles. No game data inside; the binary holds code lifted from nanpa2.exe, so local
  use only.
- 015458c, src/launch.c: game folder from --game, NANPA2_GAME_DIR, `<pref>/game-folder.txt`,
  next to the executable/.app (also `game/`, up to the .app's grandparent), ./game, then an
  NSOpenPanel picker (src/platform/dialog_macos.m, after SDL's video init so SDL owns NSApp)
  whose choice is remembered. Valid = DATA/01.GP4 and, to play, nanpa2.exe (x86_load_image
  reads it at run time), checked through fsys. `<pref>` = NANPA2_SAVE_DIR or
  SDL_GetPrefPath("nanpa2","nanpa2"); `game-folder.txt` and `nanpa2.log` sit next to the
  flag* saves without touching them. Inside a bundle stdout/stderr/SDL_Log go to
  `<pref>/nanpa2.log` (previous run: nanpa2.previous.log); NANPA2_LOG_FILE anywhere.
- Checked: `open -n` launches with NANPA2_GAME_DIR and with the remembered path (Metal,
  status file stage running, frames presented, no warnings, log written, quit on SIGTERM);
  next-to-the-app and not-found headless; the picker opens on a launch with no game folder
  (a 884x448 window at the modal panel level owned by the app), but choosing in it was not
  automated. SIGTERM does not end the app while the picker is open (SDL only queues a quit).
  A bad NANPA2_GAME_DIR in an `open -n` launch shows the error box (a 260x282 modal window,
  before SDL_Init) and exits 1 when dismissed.
- 514f15e, portability (nothing built on Linux or Windows): status.c `_environ` and
  TEMP/TMP on Windows; text.c declares MultiByteToWideChar instead of including windows.h;
  crt.c open_native lower-cases FLAG names in the save folder (the game's strings are
  "FLAG" and "flag0"; macOS saves are flag0..flag6), so a case-sensitive file system reads
  what was written (NANPA2_SAVETEST ok); Linux links libm; main.c lists DATA/*.GP4 with
  _findfirst on Windows. README "Platforms" lists what is unverified.
- Smoke: Metal build Passed; SDL_Renderer build (-DNANPA2_SDL_RENDERER=ON, also builds the
  app) Skipped: game checks passed, "desktop not visible" during its run.

## 2026-11-04 (engine stage 5: exploration bot, op 0x1f trap, Prologue checked by play)

- c05662a, d1e4920, 7a758a0, 0105525, a152d92: `NANPA2_EXPLORE=seed` (src/hle/explore.c)
  plays on its own through the dev hooks: it parses visible menus at the `04 13` fetch
  (guards evaluated with 0x4037fc, items containing 結束 skipped) and waits until the VM
  fetches the op after the menu (menus run their handlers while waiting, so the last fetch
  is not the menu); clicks hotspots from the hit-test probe (0x4071bb) and, in scenes that
  wait with op 0x11 first (OPEN1.MES), the current file's hotspots; walks on the town map
  (`04 26 3c`, button held toward a point); otherwise clicks text, grid slots and title
  buttons. It tries each choice at a place (file, offset, registers V and N) before
  repeating. Diary saves happen when it picks them (bed, 儲存), not on a schedule: seeds 41, 42 (ex5) and 52 (ex7) rewrote flag1 and flag6, the Prologue runs flag0 and flag6; loads were not counted.
  Logs `explore: reached NAME frame F f2010 N f2000 N`, `explore: stall ...` (no new script
  offset for 40000 frames), `explore: trap frame F mes M` (3158c35, x86_trap_hook) and ends
  at NANPA2_EXPLORE_FRAMES. tools/explore.py runs seeds (at most 3 jobs, each with its own
  copy of --saves) and reports coverage of DATA/*.MES. Runs repeat only roughly (host-paced
  timer); seed plus frame locate a problem.
- e3635ff, the bug it found: script op 0x1f (call a MES file, 0x406fcd; 42.MES calling
  TOMOMI.MES, the town calling TIME.MES) restores the label table with `mov cx, 0x1e` /
  `rep movsd` (0x40705e, 0x40706e), which loads only CX. The native code works because the
  message pump (0x401bf0, run at every fetch) ends in PeekMessageA/DispatchMessageA, which
  change ECX. The HLE pump kept a stale heap pointer there, so the copy ran 0x0104001e dwords
  and faulted (`x86 memory fault at 00445dbc`, eip 0). All three main-game seeds hit it
  (ex4: seed 41 just after frame 36746, seed 42 after 135823, seed 43 after 332606; each when a sub-file returned). The pump now returns
  ECX 0; NANPA2_SAVETEST also checks it (`pump selftest: ok`), run by `ctest -R selftest` (cf6dac4; fails with the fix taken out). The pump clears EDX too. With the fix, seeds 41/42 ran
  700000 frames each through TOMOMI/TIME/QUIZ_SUB/16A_SUB calls without a trap.
- Prologue by play: seeds 31-33 from an empty save folder (a new game) went START -> OPEN ->
  NAME -> S1 ... S63 -> OPEN1.MES at frames 243526, 248317 and 250105 (ex4 seed 43:
  241783), every time with F2010 = 0 and F2000 = 0 when OPEN1 loaded. So real players reach
  OPEN1 with F2010 = 0, as the static reading said. The title's Main item (START1 F1 =
  F[2026], set only by the 40.MES diary save at 0x91cc or on a second title of the session,
  re-notes "Title menu") also leads to OPEN1 with F2010 = 0.
- Coverage (DATA has 470 .MES): 140 of 470 over 11 seeds (ex3 31-33 from a new game, ex4-ex7 41-44 and 51-52 from a save with the Main item, 700000 frames each at NANPA2_TURBO=8): the Prologue (S1-S63, START, OPEN, NAME), OPEN1, 33 of the numbered scene files (01-63 with their a/b/_sub parts), TOWN/TOWN1/TIME, the quiz and sub files (QUIZ_SUB, PARA_SUB, 16A_SUB, SEN_SUB, URA_SUB) and first character files (TOMOMI, TOMOMI1, KOZUE, MINORI, AZUMI, the *_S files). Endings, SPECIAL/SPE*, TRAVEL, TENNIS and most character routes were not reached. On the fixed binaries (ex5-ex7, 5 seeds) no trap. No watchdog warnings in any run. Stalls were
  the bot cycling known offsets (OPEN1 before 0105525, NAME.MES before 7a758a0, 35.MES
  0x9df9 and TOWN.MES 0x22f1 in long main-game runs, both left again later), not frozen
  VMs: the status file kept polling (0x4075eb) with frames advancing.
- Display and sound: nothing new found; the bot does not judge pictures or audio.

## 2026-11-04 (transition speed check)

Report: transitions look very slow. Checked against the code and measured; no port bug, so no
code change.

- The transitions are timer paced and do not depend on `--cpu-mips`: the palette fade
  (0x401d30 start, 0x4028c0 steps, ext op 0x16 waits at 0x406f41) took 16 frames / 256 ms
  at 25 MIPS, 100 MIPS and unthrottled (all 121 fades of a Prologue run: 16 frames). The
  dissolve wipe 0x401a50 (script op `19 34`, 0x407382; first met in OPEN1.MES) waits 2 frames
  per step at 0x401b7b-0x401ba4 for 128 steps: 256 frames = 4.1 s, on the original as well
  (timeSetEvent(16, 16), 0x4028c0). That long dissolve is the likely complaint; it is the
  original pacing. A step's work is about 0.22 M instructions (9 ms at 25 MIPS), under its
  32 ms wait.
- CPU-bound and so throttle-dependent: (1) the black gap between a fade-out and the next
  fade-in (picture decompression 0x4075f0 and copies): first title change 9 frames at 25
  MIPS, 3 at 100, 1 unthrottled (about 3.6 M instructions); Prologue gaps without a click
  wait are at most 13 frames. (2) The wipe with a button held (bit 0x10/0x20 of 0x445d8f,
  0x401b90; the bot's 120 ms clicks trigger it): measured 28.3 M and 29.5 M instructions,
  69 frames (1.1 s) at 25 MIPS, about 0.3 s at 100.
- Accounting checked: rep string ops count one per iteration (recomp.py), about right for a
  486 (rep movsd 3 clocks per dword vs about 2.6 clocks per instruction assumed for 25 MIPS).
  The presenter cannot lengthen a timer-paced wait: ddraw_present runs at most every 16 ms,
  the timer catches up on wall-clock time (win32_run_timer, 8 ticks a call), and a present
  that blocks only eats throttle sleep (the budget allows 50 ms catch-up). The only
  per-frame GPU wait (`waitUntilCompleted`, present_metal.m) is in the luma probe, not the
  frame path.
- Original hardware estimate: fades 256-272 ms and the dissolve 4.1 s, the same as ours; load
  gaps about 1-2 frames and a held-button wipe 0.15-0.3 s on a Pentium 133-200 (100-200 M
  simple instructions per second), versus 9 frames and 1.1 s at our 486 default. Raising
  the default to 100 would match a Pentium; left at 25 (a deliberate 486 setting) pending a
  decision.
- Not checked: a windowed Metal run (scripted runs are headless); its status file
  (`present.worst_wait_ms`, `game.frame_still_ms`) would show a presenter stall.

## 2026-11-04 (voice language)

- The voice clips are spoken in Chinese (Mandarin): the Taiwan release has a Chinese dub. This
  was confirmed by ear on one clip only, `SPEECH/A/A10012.WAV`; the other clips are assumed to
  match but have not been listened to.
- Clip facts: 10,768 clips in `SPEECH/A..O`, RIFF WAV, 22050 Hz 8-bit mono, mixed `.WAV`/`.wav`
  case (handled by the case-insensitive `fsys.c`). Played by 0x403360 (see `re-notes.md`).

## 2026-11-04 (WAV format check)

- Surveyed every `SPEECH/*/*` clip and `MUSIC/*.wav`: 9824 + 771 mono 22050 Hz 8-bit, 185 stereo
  22050 Hz 8-bit (all in folder I), 1 mono 11025 Hz (`SPEECH/I/I10457.WAV`). Chunk order is
  always `fmt`, `data`, then `cue`/`LIST` after the data (voices only).
- `load_wav` (used by the voice player replacing 0x403360 and by the WAV jingles of 0x402cb0)
  was already correct: SDL_LoadWAV_RW walks chunks and reads fmt (channels, rate, bits), the
  clip is converted to S16 keeping channels and rate, and `mix_clip` resamples by rate/44100
  and duplicates mono to both sides. No sound.c behaviour changed.
- Added an offline test: `nanpa2 --wav-check FILE...` (`sound_wav_check`, src/hle/sound.c)
  decodes through `load_wav` and compares frames/rate with the header's data length / byte
  rate; ctest `wavcheck` (tests/wavcheck.py) runs it on synthetic mono, stereo, 11 kHz and
  extra-chunks-before-data files plus one real file of each format. All ok.

## 2026-11-06 (sharper text)

- Report: the Chinese text is not sharp. Cause, from 640x480 headless shots of the Prologue
  dialogue (MingLiU em 15, weight 700, 16-pixel cells; 0x401900, fonts 0x402620): the glyphs
  themselves. `load_font` took the first face of Songti.ttc that covers the Traditional
  probe characters, which is **Songti SC Bold** (face 1), and text.c drew its unhinted
  outline with a plain coverage threshold (110) and then GDI's 1-pixel bold: bold on bold,
  strokes of 1 or 2 pixels at random, thin horizontals lost or merged (要 天 報 unreadable).
- The CRT is secondary: a numpy model of `fs_crt` at 1280x960 shows the old blots stay blots
  and the new glyphs stay legible through scanlines, grille and glow. The Metal output itself
  was not captured (screen locked; windowed Metal can hang then), so present_metal.m is
  unchanged.
- Fix (29e2243, src/hle/text.c): (1) embedded 1-bit bitmap strikes (EBLC/EBDT, Apple
  bloc/bdat; index formats 1-5, image formats 1/2/5/6/7) at the em size, else the cell size;
  Apple LiSung Light, a Dynalab Ming like MingLiU and an on-demand macOS font found under
  `/System/Library/AssetsV2/com_apple_MobileAsset_Font8`, has hand-made 16 and 24 px
  strikes and is now the first choice on macOS (MingLiU first on Windows). (2) The face
  choice prefers TC/HK names and OS/2 weight nearest 400 (Songti TC Regular). (3) Outlines
  use a ridge rasterizer: ink at coverage >= 192, or >= 64 at a local maximum across the
  stroke, so strokes are 1 pixel wide before the bold. (4) A second face fills in characters
  the first lacks (LiSung has no kana and no U+FF5E ～, Big5 A1E3, used in the Prologue).
  `NANPA2_TEXT=threshold` / `=outline` keep the old raster / skip strikes.
- Checked: Prologue frames before/after (LiSung strike; Songti TC Regular with `--font`),
  `NANPA2_LOG=1` shows no missing glyphs; ctest selftest, selftest_dir, wavcheck pass, smoke
  skipped (screen locked). The name-entry grid is a picture, not text, and did not change.
  Not checked: the 24 and 32 px fonts on screen (not reached in the Prologue). Offline
  renders of the same glyph strip at 32 px showed ridge strokes too thin for the 32 px
  weight-400 font (no bold, no LiSung strike), so outlines from em 28 up keep the plain
  threshold (`RIDGE_MAX_EM`, follow-up commit).

## 2026-11-08 (opening: publisher movie skipped by default)

- The game shows two openings at launch: `LOGO.AVI`, the Taiwan publisher's movie (451 frames, 15 s; native 0x401e20, one caller, WinMain 0x402166), then ELF's
  animated logo, run by `start.mes` (0x4060d4; `elf.gp4` + `elf.ani`, then `start1.mes`).
  It was not one video played twice: `logo_play` replaces 0x401e20 and the MCI path in
  win32.c is never reached.
- 0e3be74: the publisher movie is skipped by default (binary and `.app`); `--logo` or
  `NANPA2_LOGO=1` plays it, `--no-logo` still works. Same state either way: 0x402166 drops
  EAX (`add esp, 4; call 0x402620`), 0x4ac4f8 ends 0, and 0x402620 then sets the 8-bit mode,
  palette, sound device and 16 ms timer.
- Checked: headless launch shows the ELF animation from about 1 s, then name entry (empty
  save); frame counter 247 at 6 s and 633 at 12 s, no warnings. With `--logo` the log reads
  "LOGO.AVI: ended after 451 of 451 frames" and the game's frame counter stays at 0 until it
  ends. ctest: smoke, selftest, selftest_dir, wavcheck pass.

## 2026-11-08 (double cursor)

- Report: two mouse cursors over the game. Cause: the game hides the system cursor and
  draws its own 32x32 sprite (0x4012e0, bitmaps at 0x4be900, blink 0x4ac514 from the timer
  0x4028c0). ShowCursor (IAT 0x41b11c) runs at 0x401e27 (0, the logo player 0x401e20,
  unconditional), 0x40262a (1), 0x4028ab (0), 0x402ac6 (1, shutdown): -1 in game. The
  override of 0x401e20 (src/hle/logo.c) skipped the first call, so the HLE counter ended at
  0 and SDL showed the host arrow too. No SetCursor/SetClassLong import, no WM_SETCURSOR in
  0x4021c0; the class cursor is IDC_ARROW (0x4020cd). Details in `re-notes.md`, Input.
- Fix (4e7d53a): the 0x401e20 override applies 0x401e27 first; `win32_cursor_update`
  (src/hle/win32.c) hides the host cursor only while the count is negative, the window has
  input focus and the pointer is over the game image (`present_window_to_screen`, the same
  fit with the CRT on or off, windowed or fullscreen), so letterbox borders, the title bar,
  other windows and an unfocused window keep it. Re-checked on mouse motion, window events
  and the CRT/fullscreen hotkeys. The launch dialogs (folder picker, message boxes) run before
  the window exists, while the count is still 0. The status file has
  `"cursor": {"count", "host_shown", "over_image"}`.
- Checked: windowed runs (NANPA2_LOGO=0 and =1) show `count -1, host_shown 0,
  over_image 1` with the pointer over the image and focus, and `host_shown 1` after the
  window lost focus; ctest smoke, selftest, selftest_dir, wavcheck pass. Not checked by
  eye: fullscreen letterbox (code path only).

## 2026-11-08 (build scripts)

- 09e5ca1: `tools/build.sh` (macOS and Linux, POSIX sh) and `tools/build_msys2.sh` (Windows,
  MSYS2 MINGW64 shell). Both check the tools and print the packages to install (brew; apt,
  dnf, pacman; MSYS2 pacman), never run `sudo`, check `--exe` (default `game/nanpa2.exe`)
  and pass `NANPA2_EXE`, `NANPA2_PYTHON`, `NANPA2_GAME_DIR` (default: the exe's folder) and
  `CMAKE_BUILD_TYPE` (`--release` default, `--debug`) on every run, so a reused build
  folder keeps no stale cache values. build.sh creates `tools/.venv` with
  `capstone>=5.0.1,<6` when its Python cannot import capstone; MSYS2 uses pacman's
  `mingw-w64-x86_64-python-capstone`. `--backend interp|recomp` goes to `-DNANPA2_BACKEND`
  (option not in CMake yet: until CMakeLists.txt names it, `--backend` stops with a
  message, c67a93d); only recomp, or no `--backend`, needs the exe and capstone. When the
  option lands with interp as default, the no-`--backend` case (`recomp=1`) should follow
  the CMake default.
- MSYS2 rather than MSVC: CMakeLists passes GCC/Clang flags and `recomp.c` has only been
  compiled with Clang. On WIN32 only, the target is `WIN32_EXECUTABLE` (SDL2main gives
  WinMain; configure fails without SDL2main) and named `nanpa2-port.exe`, so a copy in the
  game folder can never replace the game's `nanpa2.exe`, which the port reads at run time
  (src/main.c). `build/dist` gets it plus the DLLs it imports from `/mingw64/bin`, read
  with `objdump -p` to a fixed point (nothing is run), and a "this machine only" README.
- Checked on macOS only: a clean build folder with `--test` (fresh venv, capstone 5.0.7;
  smoke, selftest, selftest_dir, wavcheck pass), `--app` (bundle with libSDL2 and libSDL3,
  `codesign -v` ok), a re-run is `ninja: no work to do`; a missing exe and a bad
  `--backend` fail with a message; `sh -n` passes on both (shellcheck not installed).
  Linux and Windows have not been built or run.

## 2026-11-08 (reimplementation stage 1: inventory, differential harness, script expressions)

- Decision: the game logic is reimplemented by hand in C, function by function, through
  the override mechanism, until neither nanpa2.exe nor recomp.c is needed. Plan,
  inventory and method are in `docs/reimpl.md`; `tools/reimpl_status.py` measures progress.
- Inventory: the game code is 25,597 bytes in 142 functions, 0x401000-0x4077ac. CLAUDE.md's
  0x412210 included a statically linked MP3 decoder library (0x4077b0-0x412c81, already
  replaced by minimp3), and the MSVC runtime starts at 0x412c90. By module: map 63
  functions / 9,764 bytes, vm 10 / 4,295 (the loop 0x4060d4 owns all opcode handlers),
  screen 21 / 3,045, sound 16 / 2,511, startup 8 / 2,236, input 5 / 956, file 7 / 806,
  anim 4 / 687, text 4 / 675, expr 4 / 622. Many map functions are handlers in .data
  pointer tables 0x435576 (ext op 0x26 sub-ops, via 0x403a70) and 0x435dfc (cell draw,
  sprite move kinds, flood-fill direction orders).
- 83187f7: script expressions reimplemented (`src/game/expr.c`): evaluator 0x4037fc
  (operators 0x4038ae-0x4039d1, flag read 0x4039d6), constant reader 0x4039f7 (tails
  0x403a06/12/22/42), flag write 0x406455, on accessors for the VM globals
  (`src/game/vm.h`). Exact semantics and register contracts are in re-notes.md. recomp.py
  `GAME_OVERRIDES` keeps their lifted bodies as the oracle (`x86_exec(0x40000000 | addr)`).
- Differential harness (83187f7): `NANPA2_DIFF=1` runs original and reimplementation from
  the same state on every call (memory traced through rd*/wr*), then compares registers,
  flags if used and every byte written above the entry ESP; the original's result is kept
  on a mismatch. `NANPA2_DIFF_DIR` records calls, and `build/diff_replay` replays them
  without SDL, HLE or lifted code. ctest `diffcapture` (40 s headless explore run with the
  online diff) and `diffreplay` (skipped without records).
- Results. Explore runs, 300000 frames each at NANPA2_TURBO=8:
  - seed 31 (online diff): Prologue to OPEN1 at frame 264322, 3,745,731 evaluator calls;
  - seed 33 (online diff): OPEN1 at 261034, 3,913,913 calls;
  - seed 42 (online diff, from seed 32's diary save): main game, 37 MES files to
    TOWN.MES, 3,636,403 calls;
  - seed 32 (reimplementation only, no diff): Prologue to OPEN1 at 249593 and on to 33.MES.
  None had a trap, watchdog warning or stall. That makes 11.3 M evaluator, 19,758
  constant-reader and 55,929 flag-write calls compared, with 0 real mismatches. Seed 31
  logged 3 mismatches, all a harness bug: the status writer thread reads 0x446292 and
  0x4ac4ec (src/hle/status.c) while the trace was on. Since 83187f7 only the tracing thread
  is traced. Replay of the 92,932 recorded calls: 0 mismatches. Every operator token
  occurred; 0x2c and 0x2f only in the main game (50 and 10 times in seed 42). Sabotage
  check: a wrapping 0x21 gave 2 mismatching records.
- Pacing: replaced code charges native instruction estimates (22 per expression token).
  Measured per call: 116-250 native against 129-255 estimated.
- Share now replaced: 4,418 of 25,597 bytes (17.3%) and 27 of 142 functions (19.0%);
  3,796 bytes are the older HLE overrides, 622 bytes (2.4%) are game logic.
- re-notes.md: VM state table; register `@` is 0x40 (so S = register 0x13). Music ext op
  0x24 (0x4073a5) was described backwards: first argument 0 plays MUSIC n, anything else
  stops (0x4073aa `cmp eax, 0; je 0x4073b9`).
- Follow-up commit: `2d`/`2f` with a non-constant operand use the byte itself, as
  natively, instead of trapping (never met in the runs). The oracle-tag contract is
  documented for other backends.
- ctest: all 8 pass. One smoke failure during a run alongside two explore jobs passed on
  rerun (load).
- Next (docs/reimpl.md order): VM helpers 0x4061d4, 0x40741b, 0x406dc4, 0x406395, 0x4068e1;
  then jump overrides for the opcode handlers.

## 2026-11-08 (x86 interpreter backend: a binary with nothing of the game)

- Goal: a distributable engine, as xsystem4 is: the player supplies the game files,
  `nanpa2.exe` included, read at run time. Until now `tools/recomp.py` lifted the game code
  (0x401000-0x407800) into `build/gen/recomp.c`, so the binary was a translation of it.
- What was embedded: only the lifted code and the `x86_hle_targets` address list. The image
  (.text/.rdata/.data) and the import table were already loaded at run time
  (`x86_load_image`, `map_imports`); `hle_is_target` now derives the list from the code range
  and the overrides, so nothing generated is left in an interp build.
- 5af0016: `src/x86/interp.c` + `interp_core.h`, a hand-written interpreter over the existing
  runtime (cpu, x86.h flag helpers, hle_call). It decodes from emulated memory with a decoded
  instruction cache (one entry per code byte), and keeps the recompile's pacing exactly:
  X86_POLL on taken direct backward branches into game code, one count per instruction plus
  rep iterations, the pump 0x401bf0 ECX/EDX clobber (override). Coverage: every opcode form
  the lifter handles (list in re-notes "Running the game code"); others stop with address and
  bytes. `-DNANPA2_BACKEND=interp|recomp` (default interp); recomp builds also hold the
  interpreter (`NANPA2_INTERP=1`) and the lockstep check (`NANPA2_LOCKSTEP=1`,
  `src/x86/lockstep.c`): the interpreter shadows every recompiled block (X86_BLOCK) with
  journalled writes; pc, registers, flags, icount, poll countdown and every byte written by
  either side are compared at the next block, hle_call, x86_poll or return.
- One override list: `src/hle/overrides.h` (X-macro), included by the interpreter and parsed
  by recomp.py, also for the probes. 64d794d adds HLE_GAME_OVERRIDE and the oracle tag
  (X86_ORACLE_TAG, master's NANPA2_DIFF) on both backends.
- The exe is checked by PE TimeDateStamp 0x39697c6a and SizeOfImage 0xc1000 (launch.c folder
  check and the loader); a missing one is reported as "inside data1.cab on the CD (an
  InstallShield cabinet)". Extracting it ourselves: data1.cab is InstallShield 5 (`ISc(`,
  0x01005100), deflate-compressed; an IS5 header parser plus a small inflate (none in the
  tree yet) is about a day of work.
- Lockstep results (headless, scratch saves), no divergence in any run:
  explore seed 1 (60000 frames) 2.50 G instructions compared, seeds 2/3 (80000 frames,
  turbo 4) 2.91 G / 3.08 G, seeds 5/6 (300000 frames, turbo 8, through 40.MES, 41.MES and
  TOWN.MES with about 21000 frames of town) 11.59 G / 11.04 G, seed 7 after 64d794d (same
  settings, 72 MES files incl. TOWN.MES) 10.91 G; the save40 route 3.8 G (45.8 G in all)
  (it did not reach 40.MES: with the CD's FLAG6 the title leads to open.mes/name.mes, so
  the route needs a game-made FLAG6); selftest and the ctest `lockstep` run. 410-1815 block
  checks per explore run were dropped (the bot's x86_call of 0x4037fc from the 0x40616e
  probe, mid-block). A planted bug (8-bit `test` computing flags at 32 bits) was caught at
  the 4th block (0x4020fa).
- Interpreter alone: explore seed 4 (300000 frames, turbo 8) reached 70 MES files incl.
  39.MES, and seed 8 after 64d794d 71, no trap; selftest (pump ECX), smoke and wavcheck
  pass. Those cover what lockstep
  cannot: the interpreter's own HLE return path, polls, counting and probes.
- Speed, NANPA2_CPU_MIPS=0, title screen, Release, Intel Mac: 150 M interpreted
  instructions per second outside HLE calls (113 before always_inline on the hot helpers,
  64d794d), 6x the 25 MIPS pacing; x86_icount rate 569 MIPS (HLE charges included); the
  recompile 1617 MIPS by the same icount measure.
- ctest `no_game_bytes` (interp builds, tests/no_game_bytes.py): every 64-byte run of the
  exe's sections with at least 12 distinct byte values is looked for in the binary and every
  file of the .app; a planted run of .text must be found first. Two kinds of runs are left
  out as computed, not authored, and both were found shared before excluding them: the MP3
  x^(4/3) table (ISO 11172-3; minimp3 g_pow43 matched .rdata+0xaf71..0xb0f5) and linear
  ramps (SDL3 in the .app matched a 16-bit ramp at .data+0x76c46). Left out: .rdata 33004,
  .data 5061, .text 0 of 171320 runs. Result: build/nanpa2 and build/同級生２.app (7 files),
  0 matches.
- 65e8d9c: status_init set stage "running" before phase, so the writer thread could write
  the early file (no "game" section) with stage running; smoke.py read one under load and
  failed with KeyError 'game' (2 of 6 smoke runs failed while four lockstep jobs ran; the
  second showed the KeyError, the first was not captured).
- Build scripts (af109bf): no --backend means interp, no exe or capstone checks, and the game
  folder only for --test; the "never share" notes apply to recomp only. README Build
  describes both backends (CLAUDE.md and bundle_macos.sh: see the merge below).

## 2026-11-08 (merge: interpreter backend with reimplementation stage 1)

- Merge of master (b08febf) into the interpreter branch. The game overrides 0x4037fc,
  0x4039f7 and 0x406455 are `HLE_GAME_OVERRIDE` lines in `src/hle/overrides.h`; recomp.py
  reads them with `read_hooks('GAME_OVERRIDE')` (its dict is gone), the interpreter includes
  the same file. `game_call` stays first after the thunks in `hle_call`. x86.h rd*/wr* carry
  both `X86_TRACE` (differential tests) and `X86_WT` (lockstep); the str_movs fast path is
  skipped while tracing and logs its writes for lockstep. The backend switch (`x86_exec`,
  `x86_use_interp`, `x86_backend_name`) moved from x86.c to interp.c, so `diff_replay`, which
  links x86.c with its own `x86_exec`, still builds.
- Docs: CLAUDE.md Layout describes interp as the default and recomp as development and
  reference; docs/reimpl.md, src/game/game.h and units.c point at overrides.h. bundle_macos.sh
  takes the backend from CMake and prints "for this machine only" for recomp apps only.
- Results: ctest 9 of 9 in both builds (smoke skipped: screen not visible). NANPA2_DIFF=1 on
  interp, explore seed 33, 60000 frames, turbo 8 (to s9.mes): 850,851 evaluator, 965
  constant-reader and 5,655 flag-write calls, 0 mismatches. NANPA2_LOCKSTEP=1 on recomp with
  the game overrides active, explore seed 9, 120000 frames, turbo 8 (to s31.mes): 2.15 G
  instructions compared, 916 checks dropped (host call mid-block), no divergence.
  no_game_bytes: build/nanpa2 and build/同級生２.app (7 files), 0 matches.

## 2026-11-08 (reimplementation stage 2: VM helpers, jump overrides, 12 opcode handlers)

- ca615e6: the VM helpers in C (`src/game/vm.c`): block skip 0x4061d4, argument parser
  0x40741b with 0x4074a3, string argument 0x406dc4, VM init 0x406395, menu item 0x4068e1.
  Opcode handlers in C (`src/game/ops.c`): 0a 0x406491, 0b 0x4064b5, 0c 0x4064d9, 0d
  0x4064f8, 0e 0x406557, 12 0x40663e, 18 0x4069be, ext 11 0x406638, 12 0x40663b, 20
  0x4070bc, 22 0x40739b, 23 0x4071a4. Contracts and exact semantics in re-notes.md
  ("Script VM helpers", "Script opcodes").
- A second override kind for the handlers, which are entered by `jmp` and leave by
  `jmp 0x4060f9`: reaching one goes on at `pc = hle_jump(addr)`, the pc the C handler
  returns. Their oracles run until an exit (0x4060f9 / 0x40638d at the handler's ESP, or a
  `ret` above it: `x86_stop_at`). recomp.py refuses calls, fallthrough and outside direct
  jumps into a handler. Diff records gain the exit pc (N2DIFF2). docs/reimpl.md "Override
  mechanism".
- `tools/liveness.py`: interprocedural register liveness over the lifted code (function
  summaries, callee-save slots, per-lane registers, CF and ZS flags apart). At 0x4060f9
  only ESP, EBP, ESI are live, so EAX ECX EDX EBX EDI and the flags are handler scratch.
  Checked by `NANPA2_DIFF_POISON=1`, which fills every unit's scratch registers with
  garbage after it runs.
- Harness bug, present since stage 1: outputs counted only writes at or above the entry
  ESP, and the stack (0x00fxxxxx) lies below the globals, so no global write was ever
  compared. A sabotaged op 0c gave 0 mismatches. Now only stack below the entry ESP is dead
  (`diff_kept`, diff.h): the same sabotage gives 3745 mismatches, and a sabotaged string
  case of the argument parser gives 32. Stage 1's units write no globals except the flag
  write 0x406455, whose only callers (ops 0a/0b) are now in C and compared with it.
- 5d177c6: merge of master b4dcf20 (interpreter backend). The overrides are lines in
  `src/hle/overrides.h`: `HLE_GAME_OVERRIDE` for the helpers, a new `HLE_GAME_JUMP` kind for
  the handlers (recomp.py `GAME_JUMP_OVERRIDES`; interpreter `OVR_JUMP`, with the oracle
  stop checked at each fetch while `x86_stop_esp` is set). Lockstep: `hle_jump` is an HLE
  check point, an oracle stop a return point. The C hit test fires probe 0x4071bb itself,
  so the exploration bot still gets the hotspot table.
- fa0adfb: docs (reimpl.md, re-notes.md); reimpl_status.py counts the handlers.
- Note: the entry checks for jump units (no calls, fallthrough or outside direct jumps)
  are in recomp.py's check_oracles, which the default interp build does not run: build
  the recomp backend once whenever an HLE_GAME_JUMP or HLE_GAME_OVERRIDE line is added.
- Results, interp build, explore runs of 300000 frames at NANPA2_TURBO=8, all with the
  online diff except seed 32; none had a trap, watchdog warning, stall or mismatch:
  - seed 31: Prologue to OPEN1 and 40.MES (frame 267381), ends at S50.MES; records kept;
  - seed 33 (poisoned): OPEN1 at 254333, 40.MES, ends at TOWN.MES;
  - seed 32 (C only, poisoned, no diff): OPEN1 at 232351, 40.MES, ends at 41.MES, diary save;
  - seed 42 (poisoned, from seed 32's saves): 31 MES files in the main game, ends at 14.MES.
  Calls compared over seeds 31, 33 and 42: evaluator 2.45 M, block skip 1.78 M, op 0c 2.94 M,
  op 0d 713 k, argument parser 1.04 M, hit test 146 k, ext 12 174 k, op 12 71 k, op 18 47 k,
  menu item 32 k, op 0a 19.7 k, op 0b 26.4 k, string argument 3.5 k, ext 22 486, ext 11 54,
  op 0e 29, VM init 8; 0 mismatches. Ext 20 (0x4070bc) was never reached, so its handler
  (written in ops.c) is not enabled in overrides.h until a run covers it. Replay of the
  352,462 records of seeds 31 and 42: 0 mismatches.
- Short checks: recomp build with the online diff (seed 31, 6000 frames) 0 mismatches;
  NANPA2_LOCKSTEP=1 with the diff (3000 frames) and without (seed 9, 120000 frames, to
  S24.MES, 1.70 G instructions compared, 690 checks dropped) no divergence.
- ctest: 9 of 9 on the interp build (with no_game_bytes) and on a recomp build (with
  lockstep); smoke skipped (screen not visible).
- Pacing: the estimated counts are close to the native ones per call (evaluator 326-572
  against 326-559, argument parser +25%, op 0d +40%, op 0e +90% on few calls); tuning is
  left for later.
- reimpl_status: 5,267 of 25,597 bytes replaced (20.6%, was 17.3%), 33 of 142 functions;
  game logic 1,471 bytes (5.7%, was 2.4%): 8 functions and 11 opcode handlers. vm module
  849 of 4,295 bytes (19.8%).

## 2026-11-08 (reimplementation stage 3: the remaining opcode handlers, call exits, pacing)

- 8f20643: 16 handlers around native subsystem routines (`src/game/ops_sys.c`): 17
  0x406b05, 19 0x407204, 1a 0x407096, ext 14 0x406842, 15 0x406916, 16 0x406f06, 17
  0x406b76, 18 0x406bcb, 19 0x406c20, 1a 0x406c85, 1b 0x406cea, 1c 0x406d8b, 1d 0x406e11,
  24 0x4073a5, 25 0x4073cd, 26 0x4073ee. The routine they call (blits, loads, slots, music,
  the map dispatcher 0x403a70) stays native. A `native` predicate leaves the timer-bound or
  message-pumping sub-ops to the original handler: 19 sub-op 4, 1a sub-op 4, 04 16 mode 3,
  04 26 sub-ops 5/7/12, and any sub-op given as an expression.
- 7114ba0: call exits (`HLE_GAME_CALL_EXIT`). A handler that runs a block, a nested VM, a
  wait or the pump pushes the return address and returns the callee; the code after the
  call is its own jump unit (`RESUME`, `stack_words`). The oracle also stops at any jump
  unit at the stop ESP and at a call exit's callee with ra on top (`x86_stop_at`); outputs
  are dead only below the exit ESP. Units: 0f 0x40659e/0x4065ae/0x4065ed, 11
  0x4065f6/0x4065fb, ext 10 0x40660c/0x40661e, 13 0x406658/0x40666b, 14 0x406a9a/0x406aa7,
  16 0x406aad.
- 49e67a2: ext 13 0x406671 (menu loop, entry plus 22 pieces 0x4066b8-0x40683b), 15
  0x4069ce/0x4069f7 with the formatter 0x4069fd, ext 1e 0x406f7c, ext 1f
  0x406fcd/0x40704a, ext 21 0x4070d5. The harness needed three fixes for them:
  - HLE fread (0x412deb), the GP4 decompressor (0x4075f0) and sprintf wrote game memory
    untraced, so the original's load of START1.MES over the script was not undone and
    the C run parsed the new file (a stack-underflow trap). They now report to the trace.
  - The interpreter checks the oracle stop at every instruction, recomp only at dispatch,
    so after 04 1e's `mov esp, [0x445cdc]` (0x406fc2) the two stopped at different pcs.
    recomp.py now ends a block after `mov esp, [mem]` (0x40638d, 0x406fc2; also 0x404729).
  - ECX's upper half is live where ECX is scratch: 04 1f's `mov cx, 0x1e; rep movsd`
    (0x40705e) after a called file returns through 0f's `ret`. Poisoning all of ECX made
    the original copy run off memory (fault at 0x3fffffe). `diff_live_bits` keeps the
    upper half out of the poison and compares it.
- 1baf7dc: pacing. The evaluator charges per token type as the native loop runs it (6 for
  03, 14 a digit, 23 a register, 27 most operators, not 22 each), the block skip 4 a byte
  before the 01 and 6 inside. Over the runs below the C units' charges total 0.999-1.002
  of the original's (the argument parser was 135 against 99 per call).
- 96708e9: docs/reimpl.md "Stage 3", re-notes.md opcode rows.
- Not done: ext 20 0x4070bc (never reached, stays disabled); the VM loop 0x4060d4/0x4060f9
  (every iteration calls the native pump 0x401bf0 and animation step 0x403624, and text
  goes to 0x401900, so it is left for the text/anim stage). Menu pieces for inputs 2-5
  (0x40676f-0x40683b handled/waited) were never entered: the bot presses no arrow keys;
  they are the same code as inputs 0/1 with their own addresses.
- Results on 1baf7dc (the code as committed), explore runs at NANPA2_TURBO=8 with the online diff and
  poison; no trap, watchdog warning, stall or mismatch in any:
  - interp seed 31, 300000 frames: 65 MES, ends at S63.MES;
  - interp seed 33, 300000: 64 MES, OPEN1 at 284713;
  - interp seed 42 (seed 32's saves), 300000: 63 MES, OPEN1 at 298251;
  - interp seed 43 (seed 32's saves), 300000: main game, 18 MES, ends at TOWN.MES;
  - recomp seed 44 (seed 32's saves), 100000: 21 MES, OPEN1 at 90576.
  Calls compared over the five: op 0c 2.26 M, 0f 1.26 M, ext 10 763 k, ext 15 427 k, 04 19
  320 k, ext 13 8355 entries (4.64 M pieces), op 0d 505 k, 13 61 k, 14 43 k, 16 26 k, 04 26
  26 k (26 k left native), 19 5789, 04 1e 314, 21 55, 1f 21, 15 2; 0 mismatches.
- Lockstep, recomp, seed 9, 120000 frames: 1.62 G instructions compared, 894 checks dropped,
  no divergence. Sabotage: ext 13 setting [0x445304] = 2 gave 92 mismatches in 92 calls.
- ctest: 9 of 9 on the interp and on the recomp build.
- reimpl_status: 10,396 of 25,597 bytes replaced (40.6%, was 20.6%), 53 of 142 functions;
  game logic 6,600 bytes (25.8%): 9 functions and 67 opcode handlers; vm module 3,724 of
  4,295 bytes (86.7%). Handlers with native sub-ops count in full, so this is an upper bound.

## 2026-11-08 (reimplementation stage 4: the VM loop, text output, animation)

- 5731507: the harness changes the VM loop needed. recomp.py sends a call to a jump unit
  and code that falls into one (0x4060d4 into 0x4060f9) through dispatch, and an oracle
  no longer stops at its own entry when that is an exit. `x86_stop_at` never stops at
  the first check. `x86_is_oracle_pc` accepts only a tagged game code address (the HLE
  COM thunk 0x71000244 has the tag bit too, and was taken for an original). A call unit's
  `native` predicate is honoured.
- dce1208: `src/game/loop.c`, the VM loop as pieces split at its calls: entry 0x4060d4,
  loop head 0x4060f9, 0x4060fe after the pump, 0x406124/0x406134 after the button tests,
  the fetch 0x40616e (probe kept), blocks 0x4061c2/0x4061cd/0x4061d3, text
  0x406315/0x406383, input test 0x4074d5/0x4074db, frame wait 0x4075c6/0x4075d7, click
  wait 0x406acc-0x406b02, and op 16's continuations 0x406abd/0x406ac7. `src/game/text.c`:
  0x401900, 0x4063ee/0x406415, 0x406b24. `src/game/anim.c`: 0x403624, 0x403661, 0x403510
  (sub-op 4 native), 0x406dff, 0x40752c. Native screen routines go through `game_ncall`.
- Found by the long runs: 0x401900 leaves EDX = the scratch surface (0x401a2d
  `mov edx, [0x4ab058]` before the first unlock). The unit has EDX as scratch, but a Big5
  character with no delay ends its piece at the next piece (0x406383) with EDX live. A
  20000-frame run gave 4,421 mismatches in 0x406124/0x406134. A quit inside 0x4074db must
  set the flags before it unwinds.
- 3556756: the fetch's dispatch tables held as 32-bit addresses were the exe's bytes at
  0x40620b/0x406262 (ctest no_game_bytes). They are now 16-bit offsets. diffcapture now
  checks op 0c 0x4064d9. 0x4037fc is no longer compared as a unit: the C handlers call it
  directly, and the bot's evaluation at the fetch probe now runs inside a compare.
- ff7009c: docs/reimpl.md "Stage 4", re-notes.md (VM loop, text window, animation slots).
- Under the diff the fetch probe fires twice per fetch (oracle, then C). This is harmless
  for the bot, whose state updates are idempotent.
- Results, explore runs with the online diff and poison. None had a trap, watchdog warning
  or mismatch. Runs at 300000 frames:

  | Build, commit | Seed (start) | Turbo | Frames | Reached | Mismatches |
  |---|---|---|---|---|---|
  | interp dce1208 | 31 (new game) | 8 | 300000 | Prologue to S37 (40 MES) | 0 |
  | interp dce1208 | 33 (new game) | 8 | 300000 | Prologue to S38 (42 MES) | 0 |
  | interp dce1208 | 42 (seed 32's saves) | 2 | 300000 | Prologue, OPEN1 at 277010, 40.MES at 286513 (66 MES) | 0 |
  | interp dce1208 | 43 (seed 32's saves) | 2 | 300000 | main game: 40.MES at 11355, 41, TOWN at 95486, ends at 35.MES (16 MES) | 0 |

  Shorter runs:

  | Build, commit | Seed (start) | Turbo | Frames | Reached | Mismatches |
  |---|---|---|---|---|---|
  | interp dce1208 | 42 (seed 32's saves) | 8 | 300000 | OPEN1 only, two stall reports | 0 |
  | interp dce1208 | 43 (seed 32's saves) | 8 | 300000 | OPEN1 only, two stall reports | 0 |
  | recomp dce1208 | 44 (seed 32's saves) | 8 | 150000 | 17 MES, ends at OPEN1 | 0 |
  | recomp dce1208 | 45 (seed 32's saves) | 2 | 150000 | main game: 40.MES at 76691, TOWN at 121456, ends at 42.MES | 0 |
  | interp 3556756 | 34 (new game) | 8 | 100000 | Prologue to S7 | 0 |

  At turbo 8 with the diff, seeds 42 and 43 never left OPEN1. Stage 3's seed 43 did, so
  the main-game criterion is not met at turbo 8. The likely cause is the bot, not the C:
  - the timer is real time and the diff runs the whole loop twice;
  - seed 31 reaches S5 at frame 23953 plain against 33395 with the diff (stage 3: 23258
    and 26758);
  - the bot times its clicks in frames;
  - plain at turbo 8 (seed 42) and with the diff at turbo 2 (seeds 42, 43, 45), the same
    saves reach the main game.
- Compared calls over the nine diff runs:
  - loop head and pump piece 7.5 M;
  - button tests;
  - input test 5.9 M;
  - frame wait 7.2 M;
  - Big5 characters 466 k, ASCII 13 k;
  - blocks 418 k;
  - click wait 236 k;
  - slot interpreter 2358 (through 0x403510 sub-op 4);
  - VM entry once per run;
  - op 16's click continuation 2.

  Not exercised:
  - 0x4061c2 (a bare 01 block as a statement);
  - the click handler path 0x40613f;
  - ext 20.

  The called units 0x401900, 0x403624, 0x4063ee, 0x406b24, 0x406dff and 0x40752c are
  compared as part of their pieces. The sabotage shows this works: 0x4063ee writing x ^ 1
  gave 1222 mismatches in 1237 calls of 0x406383 (reverted).
- Pacing: the C units' charges total 0.9993-1.0008 of the original's per run, 1.0001 over
  the 300000-frame runs. Per piece they agree to 0.1 instruction, except the VM entry: 102
  against 103, once.
- Lockstep, recomp, seed 9, 120000 frames: 167.6 M instructions compared, 0 dropped, no
  divergence. Stage 3 had 1.62 G; the loop is C now.
- ctest: 9 of 9 on the interp build (no_game_bytes, diffcapture) and on the recomp build
  (lockstep), on 3556756.
- reimpl_status: 12,902 of 25,597 bytes (50.4%, was 40.6%), 66 of 142 functions; game
  logic 9,106 bytes (35.6%, was 25.8%): 17 functions and 88 opcode handlers and pieces.
  Modules vm, anim, text and expr are at 100%. This is an upper bound, for two reasons:
  - 19 functions (2,707 bytes) that it counts as replaced are still run natively through
    `ncall` from the C: blits, fill, palette, file load and save, picture load 0x406e20,
    the map dispatcher 0x403a70, cursor and fades. Without them, 10,195 bytes (39.8%).
  - Handlers with native sub-ops count in full.

  reimpl_status should take ncall targets as roots.
- Not done:
  - the click handler path 0x40613f-0x40616c and ext 20 0x4070bc (never reached; left
    native by predicates, and the C traps if it gets there without them);
  - 0x403510 sub-op 4 (spins on the frame counter).

## 2026-11-08 (reimplementation stage 5A: the town map)

- 94b8a1c: `tools/liveness.py` `LIVENESS_ROOTS=hex,...` (roots for code whose caller is C now, so
  `--after` still finds the call sites inside native code); `tools/reimpl_status.py` reads
  `HLE_GAME_DEAD` lines of overrides.h as barriers of the live walk.
- 4e29bbf: the map module in C: `src/game/map.c`, `map_draw.c`, `map_move.c`, `map_path.c`,
  `map_sprite.c`.
  - Sub-ops 0, 1, 2, 3, 4, 6, 8, 9, 10, 11, 13-16, 30, 32 (0x403a87, 0x403ae9, 0x403b28, 0x403bd2,
    0x403d45, 0x4042e7, 0x404f8c, 0x40500e, 0x40502c, 0x405113, 0x4053a6, 0x4053de, 0x405415,
    0x40548f, 0x405542, 0x40558f); the cell draw 0x403eff with 0x403f44-0x404056, 0x404132,
    0x4041f9, 0x404281 and 0x4040f0; the movers 0x404731, 0x404937, 0x404a9a, 0x404c9f with
    0x404e06 and 0x404e25; the walk planner 0x405966, 0x405691, 0x40562b, 0x405acd, 0x405b07 and
    its eight order cases, 0x405fb6; the sprite scripts 0x40431b, 0x404410 and the kind stubs.
  - `game_op_map` (ops_sys.c) runs the C dispatcher instead of calling 0x403a70 natively; its
    `native` predicate sends sub-ops 5, 7, 12 (and any sub-op not in `game_map_op_in_c`) to the
    original handler. 0x403a70 is not a unit: a call unit's native fallback runs nested in a host
    frame, which the pump's WM_QUIT stack reset would abandon, and only this handler calls it.
  - Units are the functions with a native caller left (0x403b28, 0x403bd2, 0x403d45, 0x403eff,
    0x4040f0, the movers, 0x404f8c, 0x40500e, 0x405691, 0x405966, 0x40431b); the rest is static C
    below them, listed as `HLE_GAME_DEAD`.
  - Harness: `game_unit.eax_live` (the movers' result is AL, and liveness conservatively says
    every register is live after them), `NANPA2_DIFF_TRACE`.
  - Left native (reach the message pump, whose WM_QUIT resets ESP): 0x4042f7, 0x404450 (kind 6,
    the player), 0x4051c2, 0x405216, 0x405597, 0x403e30, 0x40442e's alternate entry 0x404448:
    1,505 bytes, plus 0x403a70 and 0x404410 which the fallback still runs. 0x40431b runs the
    original only when its sprite's step is kind 6 (`game_native_sprite_step` looks ahead in the
    script without writing).
- 75ac4e9, 7a0009c: `NANPA2_FUZZ=N` (`src/game/map_fuzz.c`, driver in game.c): each of the first
  `NANPA2_FUZZ_CALLS` (30) compared calls of a map unit is compared N more times from random
  states (sprites, walls, view limits, walk grid, a 04 26 op's arguments written over the script,
  a cell with 0-3 sprite tiles), with the data memory, the locked screen, the stack and the CPU put
  back after each. `NANPA2_FUZZ_DUMP=1` prints the state of a mismatch, `NANPA2_FUZZ_TRACE=1`
  each state's seed.
- a518ded, the bug the fuzzing found: the native right mover's edge test adds the sprite's
  height, not its width, to x before comparing with the map width (0x404cb8, and 0x4048d7 in the
  slide to the right). 55 mismatches in 600 states of 0x404c9f, a few more in 0x404731, 0x404937
  and the units calling the movers. No explore or route run puts a sprite whose width differs from
  its height at the right edge of the map. The C keeps the quirk.
- Test route (`NANPA2_INPUT` with `sync` events, `NANPA2_INPUT_CLOCK=frames`, turbo 2): the title's
  Data Load of a diary saved in 40.MES (a copy of stage 3's seed 43 save), 40.MES's right-click
  menu (到客廳去), 41.MES's right-click menu (出去), then in the town (TOWN.MES, mati1.gp4) holds of the
  left button toward points and right clicks on roads, and a click on a house. Starting the town
  cold by naming TOWN.MES in the diary does not work: the map state record is set up by the scenes
  before it, and the native fill loops run 2^32 times over zeros. Sub-ops TOWN.MES runs (the
  fetch trace of the route): 0, 1, 2, 3, 5, 10, 12, 13, 14 and one coded `07 xx`; sub-ops 4, 6, 8, 9
  as sub-ops, 11, 15, 16 are never called by a script in these runs and only run in the fuzzing.
- Results, all with the online diff and poison (map = the units of 0x403b28-0x405966, 0x40431b and
  the 04 26 handler 0x4073ee):

  | Build, commit | Run | Frames | Reached | Mismatches |
  |---|---|---|---|---|
  | interp a518ded | route, turbo 2 | 14,000 | town, walks, right clicks, 33.MES | 0 |
  | recomp a518ded | route, lockstep | 14,000 | the same | 0 (188.8 M instructions compared, no divergence) |
  | interp a518ded | seed 64, F43 saves, turbo 2 | 300,000 | 16 MES, town at 49,773 | 0 |
  | interp a518ded | seed 62, F43 saves, turbo 2 | 300,000 | 29 MES, town at 110,748 | 0 |
  | interp a518ded | seed 63, F43 saves, turbo 2 | 300,000 | 25 MES, town at 28,321 | 0 |
  | recomp a518ded | seed 64, F43 saves, turbo 2 | 300,000 | 18 MES, town at 36,252 | 0 |
  | interp 7a0009c | route + fuzz, 6,000 states of each map unit | 14,000 | | 0 |

  No trap or watchdog warning (the fuzz runs print stall warnings while the timer is held off).
  Compared calls of one run (seed 64, interp): fill grid 21,409, overlay sprites 21,409, redraw
  21,409, cell draw 24,192, clear bit 3 24,192, area trigger and collision 21,451, sprite step
  42,818 (half of them left to the native player step), movers up 1,234 / down 1,989 / left 1,340 /
  right 1,630, plan walk 68, walk step 87, 04 26 handler 21,505 (plus 21,451 left to the native
  sub-ops 5, 7, 12). The four runs: movers 866-2,111 each, plan walk 60-73, walk step 87-140.
  Never run outside the fuzzing: the C dispatcher's cases for sub-ops 4, 6, 8 and 9 (their
  functions ran as units), sub-ops 11, 15, 16, the sprite-script kinds and the 0xc / 0xe / 0xf
  script bytes (TOWN's sprites other than the player are idle), cells with two or three sprite
  tiles. gcov of the route run with the fuzzing (before the cell-draw fuzzer): map.c 95.6%,
  map_draw.c 88.1%, map_move.c 96.3%, map_path.c 99.2%, map_sprite.c 82.4% of lines; the rest is
  the traps, those cases and the cell layers 2 and 3.
- Quit in the town (the first native fallback of a called unit that can reach the pump:
  0x40431b -> 0x404450 -> 0x40609c, WM_QUIT resets ESP inside the nested run): the route with
  `quit` 10 s of game time after the town loads exits 0 with no trap on the interp build (quit at
  +10000, +10300 and +10650 ms), the recomp build, and the interp build under the diff (185 map
  frames compared, 0 mismatches).
- ed79985: `NANPA2_DIFF_TRACE` is read once.
- Sabotage (on 4e29bbf): `+ 2` for `+ 1` in the sprite tile number of 0x403bd2 gave 1,780
  mismatches in 1,780 calls (and 1 in 0x4073ee); reverted. The fuzzing is the other sabotage
  test: it caught the right-mover quirk.
- Pacing: the C units' charges total 1.0000-1.0007 of the original's per run (map units 1.0000
  over all four, 7.1e8 native instructions in seed 64); per unit: fill grid, redraw, cell draw,
  clear bit 3, area trigger and sprite step agree to 0.1; movers within 1-3 of 100-230, plan walk
  within 0.05%.
- ctest 9 of 9 (smoke skipped: screen not visible) on the interp and the recomp build at 7a0009c
  (no_game_bytes, diffcapture, diffreplay, lockstep).
- reimpl_status: 21,145 of 25,597 bytes (82.6%, was 50.4%), 121 of 142 functions; game logic 17,349
  bytes (67.8%, was 35.6%): 31 functions and 88 opcode handlers and pieces. Map: 56 of 63 functions,
  8,266 of 9,764 bytes (84.7%, was 1 and 23 bytes, 0.2%). An upper bound, as before: 0x403a70 and
  0x404410 (53 bytes) are counted and still run by the native fallback, and the native screen
  routines the C calls (lock, unlock) are counted in their module.
- Not done:
  - The pumping chain (0x4042f7, 0x404450, 0x4051c2, 0x405216, 0x405597, 0x403e30): split into jump
    units at its eleven pump sites (0x404486, 0x4044a4, 0x4044b2, 0x4045e6; 0x4051de, 0x40520a;
    0x40522e, 0x40526e, 0x405399; 0x405598), from the 04 26 handler down, with 0x40609c (input
    module) as a piece; docs/reimpl.md "Stage 5A".
  - sub-ops 4, 6, 8, 9, 11, 15, 16 are verified by the fuzzing only.

## 2026-11-08 (reimplementation stage 5B: measurement, palette, screen routines, spin pieces)

Part of stage 5B by this branch (file, input and startup are a separate entry).

- e1fc722, 597b4e4: the measurement. `NANPA2_NATIVE_REPORT=1` (interpreter) counts the
  instructions run in 0x401000-0x4077ac outside a differential oracle and prints them by
  function at exit; the status file has `native_game_insns`. `tools/reimpl_status.py` now counts
  as needed what the C calls through `game_ncall`/`ncall`/`blit`/`colour_rect` and the original
  bodies behind `native` predicates. Stage 4's 50.4% is 36.4% on the same code (19 functions
  that were counted as done still ran, plus the fallbacks).
- 77ec0c5: the spin mechanism. The compare holds the timer off, so a C wait loop would hang
  under NANPA2_DIFF. A wait loop is a jump unit returning its own address while it waits
  (`SPIN`, `poll_self`); `game_jump` polls after the compare and the poison. 0x403510 (slot
  control) is a jump unit entered by op 1a's call exit 0x4070b6; sub-op 4 goes on in 0x4035b5.
- d4a18a5: palette tables, upload, fade start (`src/game/screen.c`), op 04 16 mode 3 waits in
  the piece 0x406f41. Both fallbacks (1a sub-op 4, 04 16 3) are gone.
- b3bb628, 7aa41a9: lock 0x401190, unlock 0x4012b0, cursor restore 0x4011f0 (units), cursor
  draw 0x4012e0, show/hide/position 0x401c40/60/80, blits 0x401430, 0x4014f0, 0x401670, fill
  0x4017b0, XOR 0x401870, picture load 0x406e20.
- d7e64a7, 252c690: fixes the long runs found: 0x401e10 must put its result in EAX (op 19 2
  stores it in S), the cursor position argument is at ESP + 4 (the write-back of y + 40 is
  compared), and the screen lock leaves ECX = 5 * pitch when the screen is shifted.
- Results (interp NANPA2_DIFF=1 NANPA2_DIFF_POISON=1, build 252c690):

  | Run | Turbo | Frames | Reached | Mismatches / traps |
  |---|---|---|---|---|
  | interp seed 31 (new game) | 8 | 300000 | Prologue to S33 | 0 / 0 |
  | interp seed 33 (new game) | 8 | 300000 | Prologue to S33 | 0 / 0 |
  | interp seed 42 (st3/saves32) | 2 | 300000 | OPEN1 ... S61, misako | 0 / 0 |
  | interp seed 43 (st3/saves32) | 2 | 300000 | main game: 40.MES, 41, TOWN at 67876, 42, 43, 35, quiz2, 36 | 0 / 0 |
  | recomp seed 45 (st3/saves32) | 2 | 150000 | main game: 40.MES, 41, TOWN, 63 | 0 / 0 |
  | recomp NANPA2_LOCKSTEP=1 seed 9 | 8 | 120000 | S24 | no divergence, 41.6 M instructions compared |

  ctest 9/9 on both builds (smoke skipped). Sabotage (cursor white 7 -> 6): 20 mismatches in
  a 20000-frame run, caught as op 04 15 writes. Pacing: insns/call of the units agree with the
  native ones to the digits for lock/unlock/cursor restore, 0.1% for the rest.
  Units never run: swap 0x401670 (op 04 1a) and 04 18 ran 154 times; swap never; click path and
  ext 20 as before.
- Native instructions, plain run seed 43 (saves32), 200000 frames, turbo 8, build 252c690: 50.45 M
  of 79.6 G charged (0.06%), 49.9 M of them the cursor redraw of the window procedure's
  WM_PAINT (0x402570 -> 0x4012e0, startup/input), then the window procedure 0.5 M, file load
  0.03 M, startup 0.001 M. Before this branch: 97 M in 5000 frames, 70% in the two spin loops.
- Not done: the dissolve wipe 0x401a50 (op 19 sub-op 4: reached twice in the seed 43 run, so it
  matters; needs pieces with its frame at the native addresses), the click path
  0x40613f-0x40616c, the map (04 26 sub-ops 5, 7, 12, 0x403a70; another agent).

## 2026-11-08 (reimplementation stage 5B, file + input + start-up part)

- f9e5923: `src/game/file.c`: 0x401000 open, 0x401050 load, 0x401160 save as C and as units
  (CALL_SYS, scratch EAX ECX EDX). The C runtime replacement is called by address through
  `game_ncall`; the stack locals, the ECX/EDX effects (ECX = buf after a plain read, the path buffer
  after a retry) and the charges follow the native code. VM entry 102 against 103 instructions,
  as before. VM entry, ops 04 1c/1d/1e/1f/21 and the picture load call the C directly.
- 466532a: `src/game/startup.c`: WinMain 0x402090 (jump unit, call exit 0x4021a3 around the
  script VM, then the shutdown), CD search 0x402590, display/DirectDraw/fonts/timer init 0x402620,
  shutdown 0x402a20, window procedure 0x4021c0 (called unit, from the HLE pump) with WM_PAINT
  0x402570. `NANPA2_HLETRACE=1` logs the HLE calls. reimpl_status: 0x401fd0 (the logo's window
  procedure, installed only by the HLE logo player) is dead.
- 895c230: merge of the screen part (the other agent's branch); the window procedure calls the C
  cursor routines. 7f7610f: WaitForVerticalBlank (ddraw.c) does not run the timer while the
  harness holds it off.
- Found by the long runs: the first 300k-frame runs gave 12 mismatches `[0x446292] want N got
  N-1` in the window procedure: its repaint waits for the vertical blank, which runs the host
  timer; a tick landed in one of the two runs of the comparison. Also found while writing it: the
  harness flags a read of any argument the original does not read (read each message argument
  only where the original does), and ECX's upper half is live (ECX ends as the original leaves it
  on each message path). A units table out of address order (binary search) made 0x401900
  "no HLE".
- Start-up is not compared by the harness (window, DirectDraw, palette and timer creation cannot
  be done twice; `uncompared()` in game.c). Checks: the HLE call trace of the first 250 calls
  (start-up and the first VM frames) equals the original's except for the lines of the units
  that replace native code and junk in unused argument slots; every run reaches the title and
  the game on the C start-up; the native counter shows no start-up routine.
- Sabotage: MOUSE_X written + 1 in the window procedure: 414 of 688 calls mismatched
  (reverted).
- Pacing (diff report, native against reimpl insns per call): window procedure 2758.7 / 2760.5
  (seed 31, 25379 calls, mostly the cursor draw), 2939.6 / 2941.4 (seed 33); VM entry 103 / 102;
  op 04 1c 331463.7 / 331452.5; op 04 1e 266 / 258; op 04 21 164 / 156 (the rest is the
  handlers' own charges, as in stage 4).
- Not compared as units: 0x401000/0x401050/0x401160 (called directly by the C, compared inside
  their callers: 04 1c, 1d, 1e, 1f, 21 and the VM entry), WinMain's two pieces (above).
- ctest 9/9 on interp and on recomp builds.
- Results (interp builds poison + diff, headless; recomp one diff run, one lockstep run), all on
  7f7610f: 0 mismatches, no trap, no watchdog warning, exit 0:

  | Build | Seed (start) | Turbo | Frames | Reached |
  |---|---|---|---|---|
  | interp | 31 (new game) | 8 | 300000 | Prologue to S20 |
  | interp | 33 (new game) | 8 | 300000 | Prologue to S29 |
  | interp | 42 (saves32) | 2 | 300000 | Prologue to OPEN1 (265541), 40.MES at 289176 |
  | interp | 43 (saves32) | 2 | 300000 | main game: 40.MES at 25940, MISAKO, TOWN at 180408, to 35.MES |
  | recomp | 44 (saves32) | 8 | 150000 | Prologue S48-S49, OPEN1 at 114114 |
  | recomp lockstep | 9 (new game) | 8 | 120000 | to S19, no divergence |

  Op 04 21 (save and load of FLAG files through the C file routines) ran 3-16 times per run and
  op 04 1c/1d/1e/1f hundreds, all inside the compare. The window procedure was compared 12.7k to
  30.4k times per run (mouse moves, buttons, activation). Units never run as units: 0x401000,
  0x401050, 0x401160 (called by the C), WinMain's pieces (uncompared). Native instructions
  (NANPA2_NATIVE_REPORT): seeds 31 and 33, 83 in 300000 frames (the VM end 0x40638d tail 2,
  81 after the op 04 1e exit at quit); seed 42 46.4 M, all in the dissolve wipe 0x401a50; seed 43
  (main game) 677 M, mostly the map module (0x403d45 208 M, 0x403b28 136 M, ...).
- ctest 9/9 on the interp build and the recomp build (lockstep, diffcapture, diffreplay).
- Not done here: 0x40609c (the map's pump, module input) stays with the map module.

## 2026-11-09 (stage 5B file + input + start-up, on top of stage 5A)

- 60e7123: merge of master 6d90a58 (stage 5A, the town map). Conflicts only in includes, CMake
  sources, game.c (`uncompared()` beside `fuzz_unit`, both apply) and the docs. The logo window
  procedure 0x401fd0 moved from reimpl_status `DEAD_ENTRIES` to `HLE_GAME_DEAD` in overrides.h.
  No units.c rows needed changing: the 5B rows use the shared macros, which carry `eax_live`.
- reimpl_status on the merge: 22388 of 25597 bytes (87.5%), 126 of 142 functions; reimplemented
  game logic 18592 bytes (72.6%); startup and file 100%, input 93.3% (0x40609c left).
- ctest 9/9 on interp and recomp builds (smoke skipped, screen not visible).
- Runs on 60e7123 (interp: diff + poison + native report; recomp: one diff run, one lockstep
  run), all 0 mismatches, no trap, no watchdog warning, exit 0:

  | Build | Seed (start) | Turbo | Frames | Reached | Native game insns |
  |---|---|---|---|---|---|
  | interp | 31 (new game) | 8 | 300000 | Prologue to S38 | 52 |
  | interp | 33 (new game) | 8 | 300000 | Prologue to S37 | 47 |
  | interp | 42 (saves32) | 2 | 300000 | MISAKO, TOWN at 281806, 40.MES | 47.6 M |
  | interp | 43 (saves32) | 2 | 300000 | main game, the girls' _S files, 39.MES | 64.9 M |
  | recomp diff | 44 (saves32) | 8 | 150000 | to OPEN1 | |
  | recomp lockstep | 9 (new game) | 8 | 120000 | to S24, no divergence | |

- Save/load: op 04 21 ran 4-18 times per run inside the compare; op 04 1c 54 times in the recomp
  run. Window procedure compared 12.8k-30.5k times per run, native/reimpl insns per call 2171.2 /
  2172.9 (seed 31), 3572.1 / 3574.0 (seed 43).
- Native report: the 44-50 instructions at 0x406f7c in the new-game runs are one per op 04 1e
  call, the `jmp 0x4060f9` at 0x406fc8 the C handler returns to (not the quit path, as the
  2026-11-08 entry guessed); 0x40638d likewise. Seed 43: wipe 0x401a50 46.3 M, the map pump
  0x40609c 12.8 M, 0x405216 4.3 M, 0x404410 0.7 M, then 0x40431b, 0x4042f7, 0x403d45 (from the
  native map callers), 0x40500e, 0x4073ee, 0x403a70, 0x405597.
- Sabotage on the merge (window procedure writes MOUSE_X + 1): 558 of 920 calls mismatched.
- Start-up HLE trace (first 250 calls) against the pre-5B baseline: equal apart from the units'
  own lines and unused argument slots, as before.
- The map pump chain 0x4042f7, 0x404450, 0x4051c2, 0x405216, 0x405597, 0x403e30 is not made
  splittable by this work. The window procedure in C runs under the HLE pump 0x401bf0; the
  obstacle is 0x40609c itself (a pump that takes a message, and its WM_QUIT `mov esp,
  [0x445cdc]`), which still needs the jump-unit split at each pump site.

## 2026-11-09 (reimplementation stage 6: the last native game code)

- 146ef80: the dissolve wipe 0x401a50 (op 19 sub-op 4) as C pieces (`src/game/wipe.c`, written by a
  helper): set-up and passes, the rest after the pump (return 0x401b90), the frame at the native
  stack addresses, 166,485 native instructions charged exactly, op 19's native fallback removed.
- d5eb14f, 169d988, caf1e5a, 8fe7cce, bbe9b18: the 04 26 pump chain (`src/game/map_pump.c`): the
  handler 0x4073ee, the pump 0x40609c/0x4060a2 and every pump site's return address (0x40448b,
  0x4044a9, 0x4044b7, 0x4045eb, 0x4051e3, 0x40520f, 0x405233, 0x405273, 0x40539f, 0x40559d) are jump
  units joined by call exits; the handler's tail is 0x407410. Replaces 0x403a70, 0x403e30, 0x4042f7,
  0x40431b, 0x404410, 0x404450, 0x4051c2, 0x405216, 0x405597. The VM's click handler path
  0x40613f-0x40616c (0x40614a, 0x406153, 0x406164) and ext 20 0x4070bc are C. The tail
  instructions (0x40638d, 0x406393, 0x40472f, the `jmp 0x4060f9` at 0x406fc8) run in
  `finish_exit` after the comparison. NANPA2_START_ORACLE and ctest `start_trace` compare the
  start-up's HLE call trace with the original WinMain's (171 calls, identical). Details:
  docs/reimpl.md "Stage 6", re-notes.md.
- Findings: no shipped script uses ext 20 or ext 1a (tools/mes_ops.py: 0 of 470 MES files), so the
  click handler path is reached only with NANPA2_CLICKTEST=1 (dev.c). 04 26 sub-op 7 is used 9 times
  and leaves the AX of its last sprite step in S; nothing reads it, AX is not compared there
  (`diff_ax_unspecified`). A 5B-style oracle for the handler failed at the first town call: with the
  stop ESP one word below the entry (the pushed ESI) the "ESP above the stop" rule ended the oracle at
  the argument parser's return; the tail piece moved to 0x407410 (after `pop esi`). A click-path
  piece trapped under poison because 0x406124/0x406134 had EDI scratch while the path keeps the handler in
  it. Route runs of the town are lotteries under the compare at turbo 8 (a click at the title that
  lands before the menu is up leaves the route waiting for ever): turbo 2 works.
- Native instructions (NANPA2_NATIVE_REPORT=1, plain, 300k frames, seed 43 from saves32, turbo 2):
  before 53,166,107 in 14 functions (0x401a50 46.3 M, 0x40609c 4.8 M, 0x405216 1.6 M, 0x404410,
  0x40431b, 0x4042f7, 0x403d45, 0x40500e, 0x4073ee, 0x403a70, 0x405597, 0x406f7c 81, 0x407204, 0x40638d);
  after **0** in 0 functions, also for seed 42 (plain, 300k frames, reaches TOWN at frame 26678 and
  stays in it). reimpl_status: 25,597 of 25,597 bytes, 142 of 142 functions (was 22,388, 126).
- Verification on bbe9b18 (diff + poison, headless; units compared, mismatches, charge = C insns over
  native over all compared units; interp unless noted):

  | Run | Frames | Reached | Mismatches | Charge |
  |---|---|---|---|---|
  | seed 31, new game, turbo 8 | 300000 | Prologue to S22 | 0 | 1.00066 |
  | seed 33, new game, turbo 8 | 300000 | Prologue to S20 | 0 | 1.00057 |
  | seed 42, saves32, turbo 2 | 300000 | OPEN1, 40.MES | 0 | 1.00069 |
  | seed 43, saves32, turbo 2 | 300000 | main game, MISAKO, TOWN at 189707, 24830 04 26 calls, 174335 pumps, 36 wipe pieces | 0 | 1.00004 |
  | recomp seed 44, saves32, turbo 2 | 150000 | to TOMOMI | 0 | 1.00032 |
  | recomp lockstep seed 9, turbo 8 | 120000 | to S23 | no divergence (thin: no native game code runs) | |
  | town route (turbo 2) with NANPA2_FUZZ=100, three runs | route | mati1, walk, clicks, right clicks, quit | 0 | 1.0005 |

  The fuzz (`NANPA2_FUZZ`) compared each pump site's piece 2900 times, the handler 3000 (op 7 included)
  and ext 20 3000 states, all equal. The click handler path under NANPA2_CLICKTEST (seed 31, 20000
  frames): 159 handler runs, 0 mismatches. Earlier runs on d5eb14f/169d988 (before the tail piece
  moved): seeds 42 and 43 reached the town, 0 mismatches after the fix. Watchdog warnings (main thread
  stalled over 3 s in one compare: the wipe's first piece, a fuzz state) occurred in the seed 42/43 runs
  (1 and 3) and the fuzz routes (29), never a trap or a mismatch; none in the others.
- Sabotage: the event check's mouse test off by one (`+ 0x61` for `+ 0x60` in `game_ev_pumped_b`,
  0x405273): the route with the fuzz reported `diff MISMATCH 00405273 map_event_pumped_b` (reverted).
  The wipe: see 146ef80's helper (a source column off by one at pixel 100: 5 mismatches in
  `wipe_pumped`).
- ctest 10/10 on the interp and the recomp builds (smoke skipped, screen not visible), including
  `no_game_bytes` and the new `start_trace`.
- Left: nothing of the game code runs natively. The exe is the oracle of the differential test (and,
  in the recomp build, the lifted reference); a build that drops it needs the state to move from
  emulated memory into C structs (not done). Not exercised by any run: ops 04 1a (swap rect; no script
  uses it), the menu input blocks 2-5 of 04 13 (`handled2`..`waited5`), the click handler path in a
  real script, and the event check's pieces in an explore run (the fuzz and the route cover them).

## 2026-11-09 (nanpa2.dat: running without nanpa2.exe)

- 9355a78: after stage 6 no native game instruction runs, but the interp build still loaded the
  whole nanpa2.exe because the C reads the exe's initialized data at its native addresses. A
  local asset `nanpa2.dat` now holds that data and no code: `src/x86/asset.c` (format and writer
  and loader; the source holds two address ranges, 0x400000-0x401000 and 0x41b000-0x4ab000, no
  bytes of the exe), `src/assets.c` (lookup: game folder, then save folder; made at the first
  launch from the exe, or `nanpa2 --make-assets [--game] [--exe] [--out]`; `--exe`, the recomp
  build and the oracle modes `NANPA2_DIFF`, `_DIFF_DIR`, `_FUZZ`, `_START_ORACLE`, `_INTERP`,
  `_LOCKSTEP` load the whole exe, with a clear error if it is missing; neither file: "no
  nanpa2.dat and no nanpa2.exe"), launch.c accepting a folder with either. Gitignored, never
  shipped. Format: re-notes.md; what it holds and how that was found: docs/reimpl.md "The
  asset". 66 ranges, 96,053 bytes (95,213 of data: 546 header, 82,497 .rdata, 12,170 .data; zero
  runs of 32 bytes or more are recorded, not stored). The interpreter's decoder refuses to run
  when the code is absent (`x86_code_absent`).
- `NANPA2_IMAGE_TRACE=FILE` (x86_ptr marks the PE image bytes touched): in five headless runs
  with the exe hidden (seed 31 new game 60k frames, seed 43 saves32 30k, seeds 42 and 43 300k
  frames at turbo 2, 19 and 70 MES files, the town reached) no byte of 0x401000-0x41b000
  (game code, MP3 library, C runtime) or of .rsrc was touched, the decoder never ran, and the
  header page was touched at 8 bytes. The whole of .rdata and the initialized .data is kept anyway
  (paths no run took); the bss tail is zero at load.
- Verification (interp build, headless, scratch save folders, a folder of symlinks without the
  exe, the real game/ untouched):

  | Run | Result |
  |---|---|
  | ctest interp, 10 tests (smoke excluded: it opens a window) | pass, incl. `no_game_bytes` and the new `asset_only` |
  | ctest recomp, 9 tests (smoke excluded) | pass (the recomp build always loads the exe) |
  | `asset_only`: 24 s of game time, exe hidden vs `NANPA2_INTERP=1` | 548 HLE calls and 5 screenshots equal; the asset holds no 64-byte run of .text and no range over 0x401000-0x4077ff |
  | explore seed 43 saves32, 8000 frames, turbo 2, exe hidden vs whole exe | same 4 MES files at the same frames, 0 native, exit 0 |
  | explore seeds 42, 43, 300000 frames, turbo 2, exe hidden | exit 0, 0 traps, 0 warnings, `native: 0`, seed 42: 19 MES, town at 26672 (left it), seed 43: 70 MES |
  | same two seeds with the whole exe (`NANPA2_INTERP=1`) | exit 0, 0 traps, 0 warnings, `native: 0`, seed 42: 12 MES, town at 26672 (stalls in it), seed 43: 64 MES |
  | seed 42 with the whole exe twice more, 100000 frames | also diverge from each other after quiz_sub.mes (frame 27679): 33.mes at 31743 against 21.mes at 42927 |

  Long explore runs are not repeatable (the game timer is host-paced, the bot clicks by frame):
  the asset and exe runs of seed 42 are equal through quiz_sub.mes at frame 27634 and then take
  other branches, the pair of seed 43 differs from open.mes on (frame 547 against 548), and the
  two further exe runs of seed 42 differ from each other and from both just as much. The equal
  screenshots at fixed frames the task asked for are therefore not available from these runs; the
  equality evidence is the scripted game-clock run of `asset_only` and the image traces (no
  byte outside the kept ranges was read, so memory is the same for every byte the runs used).
  Turbo 8 with scripted clicks was tried as well and differs in the number of repeated timer
  and surface calls even between the asset and the exe, so it was not used.
- Still needs the exe: `--exe`, the recomp build, the lifted originals of the differential
  harness (`NANPA2_DIFF`, `_DIFF_DIR`, `_FUZZ`, `_START_ORACLE`) and the lockstep comparison, as
  the oracle of the reimplemented units; nothing the player runs does. With the default
  `NANPA2_GAME_DIR` (`game/`) ctest now writes `game/nanpa2.dat` (first launch behaviour).

## 2026-11-09 (refactor R0: deterministic replay; R1: state into structs, started)

Plan and details: docs/refactor.md. The user approved turning the C reimplementation into a
free-standing engine in stages (R1 state into structs, R2 ordinary calls, R3 real-time pacing,
R4 direct platform calls); the byte-for-byte differential harness stops applying, so replay is
the check.

- 9bb18d5 (R0): `NANPA2_DETERMINISTIC=1`, `--record FILE`, `--replay FILE` (src/hle/replay.c).
  The game clock is `x86_icount / 25000` ms (25 MIPS, turbo and the throttle cannot change it),
  scripts and the bot run at every pump, outside messages are stamped (tick, pump of the tick).
  Checkpoints every N ticks hash the screen, palette, the 0xc00 save image, MES files and opens,
  VM opcode fetches, the FLAG files (`li.*`) and the raw emulated memory by 4 KB page, heap, stack,
  registers (`raw.*`). Replay stops at the first difference (exit 3, key named);
  tools/replay_diff.py compares two recordings; ctest `replay_det` (records at test time twice,
  replays, two tampered recordings must diverge). Two recordings of the same input were identical
  files at once, at 25 MIPS and unthrottled.
- 62f125d: the bot's expression evaluation (x86_call 0x4037fc) advanced `x86_icount`, 77
  instructions a menu: a replay (no bot) drifted. Found by replaying a recorded menu run.
- Golden set (pre-refactor build, scratchpad, not committed): newgame (seed 31, 270000 frames,
  271 checkpoints, Prologue to OPEN1), town (saves32, seed 43, 300000 frames, 301, TOWN at frame
  263535), saveload (seed 41, a title load to OPEN1, 33), menu (seed 44, 40000 frames, 161).
  Each replays identically on the pre-refactor build (twice) and with `--exe`.
- bef599a (R1): `x86_alias` (src/x86/x86.c) backs a range of the data area with a C
  object, so converted and unconverted code, the HLE layer and the script VM see the same bytes;
  src/game/state.h structs `save_image` (0xc00, the FLAG file layout, static-asserted offsets),
  `vm_rt`, `screen_state`, `anim_state`; the 0x4452xx text window, VM flags, registers, flags,
  frame counter, input bits, fade/cursor words and the rest named there are `SV()/RT()/SC()/AN()`
  field accesses. Every chunk matched all golden checkpoints, `li.*` and `raw.*`.
  Frozen: the diff harness, fuzz, lockstep, recomp tests (ctest option NANPA2_FROZEN_TESTS, off):
  they compare `x86_mem` directly. Kept and passing: selftest, start_trace, no_game_bytes,
  asset_only, wavcheck, replay_det.
- A mistake caught by replay: a name-based rewrite turned `CURSOR_SHOWN` of screen.c and
  startup.c (0x4ac510) into the field at 0x445ce0; raw.data differed at the first checkpoint.
  The converter now resolves names per file (tools: scratchpad convaddr.py, not committed).
- What is left: docs/refactor.md "Still in emulated memory" (script and map buffers, sprites,
  sound words, HLE heap objects), then R2-R4.

## 2026-11-09 (refactor R1 finished: all game state in one C object)

Details: docs/refactor.md "R1", re-notes.md "The data area as one object". ae7b33c: the one object, the data check, replay_det with it. 2852980: palettes, cursor bitmap, map variables, history by field; docs.

- `struct game_state G` is now the whole mutable data area 0x431000-0x4c0000 in native layout,
  one alias (`x86_alias`, fast path in `x86_ptr`): the script image, segment 1 (map), the picture
  buffers, the ANI file, the map's cells, shadow, work planes and variables (`map_data`), the
  cursor bitmap, the sound words, the screen/palette/fade, the VM block, the save image and
  the animation slots. The layout is native, so unchecked script indices (animation slot,
  surface number, tile number, segment offsets) read and write what they did.
- Converted to named fields: the three palette tables and the fade (screen.c, anim.c: `memcpy`
  and arrays instead of `rd32` loops), the cursor bitmap and its saved background, the map
  variables (`MV()`; ~100 accesses in map*.c), the position history (`hist_*`).
- New check `NANPA2_DATA_CHECK=1|2` (src/x86/x86.c): lists data-area bytes changed outside an
  alias; `=2` exits 4. ctest `replay_det` runs its replay with `=2`. Result on all four goldens:
  0 ranges (before the single alias: 143 ranges, the script, segment 1, the picture buffers, the
  ANI buffer and the cursor bits).
- Not moved, and why: pointers stored in state and the register-style address arithmetic of the
  map, anim and op code (R2), the C runtime and DirectDraw objects on the emulated heap (R4),
  the FLAG file I/O through the generic file loader (R4, it needs the C runtime thunks to go),
  the read-only tables of `head` and the tile offset table (asset data). See docs/refactor.md.

## 2026-11-09 (refactor R2: ordinary C calls instead of register and stack conventions)

Design and details: docs/refactor.md "R2". Commits (oldest first): 3a21c09 (frozen differential
machinery, recomp backend and lockstep removed), 775b2fe (`src/game/ctl.h`: COST/POLL, pump, quit,
catch, `hle_invoke`), c69faf3 (graphics leaves typed, direct wipe and fade wait), 343e003 (the script VM
in direct style: loop, blocks, text, waits, menus, control ops), f77e95d (town map: typed views, direct
pump chain), 7cc8086 + 1253106 (expressions, files, animation slots, start-up as typed C, `game_main`),
merges 14548f9, 3c2e74e, cd7056b, 2e9567e (the subsystem ops direct; interpreter, unit table, piece
dispatch removed), 7a0501b (docs, build scripts, tests).

- Control flow: a wait is a C loop that calls `ctl_pump()`; WM_QUIT, the end of the script and opcodes that
  end the VM unwind with `ctl_quit()` (longjmp to the one `ctl_catch`); a script restart longjmps to the top
  of the VM loop. Chosen over a state machine (rewrites the pieces into states) and a coroutine or thread
  (platform-specific, and SDL wants events on the main thread).
- Pacing unchanged: every `X86_COUNT`/`X86_POLL` kept (as `COST`/`POLL`) in the same order relative to
  the pump and the HLE calls. All four golden replays (re-recorded on d79f8e0 into `golden_r2`) match every
  `li.*` key plus `raw.icount` and `raw.data` after every chunk; `raw.cpu/stack/heap` change by design.
- The native-address dispatch is gone: `game_units`, `hle_jump`, `overrides.h`, the x86 interpreter and
  decoder. Nothing executes native game code. Removed with them: `NANPA2_DIFF`, fuzz, lockstep, the recomp
  backend, `tools/recomp.py`, `liveness.py`, `reimpl_status.py`, ctest `start_trace` (oracle comparison).
- Still on the CPU model: `hle_invoke` and the emulated stack frames for the HLE thunks (R4), the aliased
  address space, `top_esp` (dead, kept for `raw.data`), the instruction counter (R3).
- Tools: `NANPA2_REPLAY_CHECK` takes key prefixes; `NANPA2_HLETRACE=2` prints the instruction count per HLE
  call (equal traces <=> equal pacing and platform traffic; saveload and menu identical to d79f8e0 once
  the calls that became direct C and the stack garbage of parameterless thunks are dropped).
- Found on the way: the exploration-free replay is the only check that catches a charge off by one
  (a missing 3-instruction native prologue in `game_main` showed up at checkpoint 0 as raw.icount -3).

## 2026-11-09 (refactor R3: real-time pacing, no instruction charges)

Design, derivation and numbers: docs/refactor.md "R3". Commits (oldest first): c772d12 (`NANPA2_TIMELOG`
probes, to measure), 4aae940 (the time model: `src/game/pace.h`, `src/hle/clock.c`; `COST`/`POLL` and
about 900 charge sites, `x86_icount`, `top_esp` writes, the ECX/EDX zeroing after the pump removed),
1cf2240 (docs, `replay_diff.py --sequence`, `NANPA2_REPLAY_CHECK=none`, `replay_det` checks the clock).

- Measured first: a profiling build attributed every old charge to its function while the four
  golden_r2 runs replayed. 89 % of the game time is the pump spinning on the 16 ms timer (a wait), 3 %
  more the waits' loop bodies, 3.8 % expression evaluation, 1.1 % block skipping, 1.1 % GDI text, 0.5 %
  GP4 decoding; the map, animation steps, cursor, fills together under 0.1 %.
- Model (microseconds, each the old instruction cost / 25): pump 21, opcode 2, expression 9, block skip
  1 + 0.2 a byte, glyph 200 a byte, character cell n*n/3, decode (70 a compressed byte + 1.2 an output
  byte)/25, blit pixels/100 (keyed /25), dissolve pass 25 x (260 + 6.4 a mask bit), fade set-up 290, a
  non-pumping spin round 1. Alongside the old charges on the four goldens: newgame +1.2 %, town +1.4 %,
  saveload -1.3 %, menu -0.1 % of game time.
- Clock: virtual microseconds in `clock.c`; the 16 ms timer follows real time x `--speed` /
  `NANPA2_SPEED` (1.0 = the original; `NANPA2_TURBO` is an alias; `--cpu-mips` is gone), or the virtual
  time in deterministic mode and at speed 0. The throttle makes the work costs real by sleeping.
- Found on the way: two waits spin without a pump on a flag the timer callback sets (the animation slot
  wait 0x4035b5 and the fade wait 0x406f41); their only timer came from the polls, so removing `POLL`
  hung the first run at frame 0. Each round now passes `PACE_SPIN_US`.
- Same scripted input, 90e7de9 against R3 (deterministic, game ms = real ms at speed 1.0): fade 258-272
  against 258-272, dissolve 4094/4096 against 4094/4096, with a button held 949 against 957, a step of
  the player 48 against 48, the load scenario to its first wipe 12811 against 12798 (the table is in
  docs/refactor.md). Real-time headless runs of the same script (host clock): 90e7de9 dissolve 4098 ms, title to open1.mes
  2297 ms; R3 at speed 1 4094 and 2312 ms, 18.5 s of game time in 18.7 s; at speed 4 3.78x, at 8 7.11x,
  no warnings. No save image is comparable across the builds (see docs/refactor.md).
- Goldens re-recorded on 4aae940 into `golden_r3` (kept outside the repository): newgame seed 31, saveload
  seed 41, town seed 48 (330000 frames; the old seed no longer reaches the town), menu seed 46 (S46-S53).
  Each replays twice with every checkpoint matching (`li.*`, `raw.clock`, `raw.data`, `raw.heap`,
  `raw.stack`), identical re-recordings, 0 unaliased ranges.
- Content check: saveload (same path) is identical in MES files, FLAG state, quiescent screens and last
  palette. The old recordings' messages replayed on R3 stay in step for 12 MES files (9 in menu) and
  then desynchronise, and the exploration bot, which decides from pump counts, takes other paths in long
  runs: those can neither show nor rule out a content difference, so the diff of the removal was reviewed
  by hand (only whole-line charges and 18 `if` lines that guarded only a charge went; two spin waits
  gained a charge).
- ctest (selftest_dir, selftest, replay_det, wavcheck, no_game_bytes, asset_only) pass; plain headless
  run, 300000 frames of seed 48 from the town saves, 0 warnings or traps.

## 2026-11-09 (refactor R4: a free-standing C engine, no emulated PC)

- 8123dcd: the checkpoint key `raw.gdata` (hash of `G` with the platform's handles, lock descriptor and locked
  pixel pointers zeroed), added on the R3 code; `--replay` with `--record` writes the replayed messages. The
  four `golden_r3` recordings replayed on it with every old key matched and re-recorded as `golden_r4`
  (messages, `li.*` and `raw.clock` equal to the old ones, plus `raw.gdata`): the new definition proven
  before anything changed.
- aee4867: files and sound are direct calls. `src/platform/files.[ch]` keeps `crt.c`'s path rules;
  `file.c` is plain C (same open attempts in the same order, the decompressor input length of the native
  block `((size + 8 + 15) & ~15) - 8` for the pacing charge). Found: 0x401050 tests the name against "FLAG"
  (0x431058), not "DATA" (re-notes.md "Findings of R4").
- b168ef3: DirectDraw, GDI text, Win32 and the 16 ms timer are platform calls (`platform/surface.h`,
  `text.h`, `window.h`); `hle_invoke`, the thunks and the COM objects are gone; the timer tick (0x4028c0) is
  `src/game/timer.c`. The primary surface keeps the handle value of the emulated heap (register S).
- 5d5612e: `src/x86` removed: `G` is initialised from the asset image (`src/asset.c`) and stands alone;
  `game/gmem.h` indexes it by G addresses (the original's numbering) and traps outside it; `src/trap.c`
  (`game_trap`: the native address, the frame and script). FLAG files go through `state_save_export/import`
  (the segment bases are not taken from a file). Dead `native_pc`/`exit_pc`/`loop_pc` parameters and
  `unused_top_esp` removed. `raw.cpu/data/heap/stack/pg` and `NANPA2_DATA_CHECK` retired.
- f40feb6, b55d64f, f1bd5f8, and the documentation commits: `src/hle` dissolved into `src/platform` and
  `src/dev`; build scripts without the recomp/capstone leftovers (`tools/build.sh`, `build_msys2.sh`);
  `NANPA2_LOAD_EXE=1` for the exe-whole data; self-test through the serializer.
- Removed: `src/x86/` (x86.c, x86.h), `src/hle/` crt.c, ddraw.c, hle.c, hle.h and the import/COM tables of
  win32.c; src went from 15412 to 14226 lines (+1872/-3058 over the stage), no file of the game folder touched.
- Checks: the four `golden_r4` replays match on every chunk and twice on the final build (271/331/33/161
  checkpoints, `li.*`, `raw.clock`, `raw.gdata`); the FLAG0 and FLAG6 the final build wrote in all four runs
  are byte-identical to 5b37d43's; the install's own FLAG files (copied) loaded in a bot run recorded on
  5b37d43 and replayed on the final build (33 checkpoints); a plain 300000-frame run of seed 48 from the
  town saves: 4800.3 s of game time in 45 s, TOWN.MES 7 times, 513 file opens identical to 5b37d43's, 0 traps or
  warnings; ctest 7 of 7 (smoke skipped; replay_det 5.6 s instead of 35).
- Portability: 64-bit little-endian assumed and checked at build time; strict-width syntax pass over every
  source (details in docs/refactor.md "Portability audit"). Linux and Windows builds are still untested.

## 2026-11-09: asset version 2, the initialised .data only

- `state_init` is the only reader of the asset image and copies 0x431000-0x4c0000; `g_ptr` traps below
  0x431000, so the PE header page and .rdata 0x41b000-0x431000 were dead weight. With the image outside `G`
  poisoned before `state_init`, the four `golden_r4` replays matched every checkpoint (271/331/33/161).
- `nanpa2.dat` version 2 keeps 0x431000-0x4ab000 only (12,170 data bytes in 18 ranges); version 1 files are
  still accepted (a superset). `asset_only` checks the version, that no range lies outside the data, and the size.

## 2026-11-09: G defined in code, asset version 3

- `state_defaults` (src/game/state.c) sets every start value of G the engine reads, as typed fields
  (`data_head` split into `cd_prefix`, `msg_no_cd`, `msg_no_cd_title`; `save.n.w_308` named); the tile offset
  table and the 16 base colours are computed. Details and addresses: docs/refactor.md "G from code".
- `nanpa2.dat` version 3 holds only the game's own text that stays out of the source: 124 bytes in 2 ranges
  (the no-CD message and title, and a 7-byte text at 0x445c71). Versions 1 and 2 still load.
- Checked against the whole image (temporary diff of G vs the exe's data): differences only in the unread
  ranges. The four goldens match on `li.*` and `raw.clock` (271/331/33/161) twice with a v3 asset, and once
  each with a v1 and a v2 asset. The old recordings' `raw.gdata` hashed the unread bytes and no longer matches.
- `no_game_bytes` also looks for every 16-byte run (8 or more different values) of the exe's .data in the
  binary; the allowed ones are the file-name constants (`start.mes`, `flag0`, `MUSIC\%02d.*`, `SPEECH\A\A10930.WAV`).
  `asset_only` checks the range list and size and that a version 2 file is still used.

- `no_game_bytes` 16-byte check: it also failed on the app's libSDL3.dylib, which holds the 16-bit ramp 0, 4, 8, ...
  that the exe's MP3 library data has (.data 0x4a7c46). Linear ramps (computed tables) are now left out of the 16-byte
  check as they are of the 64-byte one; our own binaries are still scanned for every other run.

## 2026-11-09: no asset: the engine needs neither nanpa2.exe nor nanpa2.dat

- The no-CD box (0x402090) shows our own wording ("Game CD not found." / "找不到遊戲光碟。", title 同級生２, UTF-8;
  `win_message_box` prints it and, for a watched launch, shows an SDL box); nothing reads 0x431140-0x431160.
- The 7 bytes at 0x445c71 start as zero. A probe on every `g_ptr` access of 0x445c70-0x445c78 logged none over
  the four goldens and an exploration-bot sweep (see the summary below); no script touches it.
- Removed: `src/asset.[ch]`, `src/assets.[ch]`, `--make-assets`, `--exe`, `NANPA2_LOAD_EXE`, ctest `asset_only`
  (a nanpa2.dat of any version in the folder is simply not opened). `launch.c` accepts a folder with DATA/01.GP4.
  New ctest `standalone` (tests/standalone.py).
- The four goldens (`li.*`, `raw.clock`) match twice (271/331/33/161) in a folder of symlinks with no exe and no
  nanpa2.dat; the FLAG files they write are byte-identical to the previous build's on the same inputs (16 of 16).

## 2026-11-09: layering cleanup: platform no longer includes dev

- `src/hooks.[ch]`: one table of function pointers (`struct dev_hooks`) with inline `hook_*` wrappers that are
  no-ops while a hook is NULL. `platform/{files,clock,window,surface,sound,logo}.c` and `trap.c` call them instead of
  `dev/dev.h`, `dev/replay.h` and `dev/status.h`; `dev_install_hooks` (`dev/dev.c`) fills the table at the top of
  `main`. `game/state_view.c` took `replay_hash` from the dev layer; the hash now lives in `src/hash.[ch]`
  (`hash_bytes`, same function, checkpoints unchanged).
- New ctest `headers` (`tests/headers.py`): every header in `src/` compiles alone, nothing but `dev/` and `main.c`
  includes a `dev/` header, game code includes no SDL. All headers already stood alone.
- Four goldens (`li.*`, `raw.clock`) match: 271/331/33/161 checkpoints. Removed the "Left" item about
  `files.c` in `docs/refactor.md`.

## 2026-11-09: right-click latency investigation (no pacing change)

Report: the right-click menu sometimes appears late. Measured headless in real time (R3 pacing, speed 1)
with the new dev hooks: `NANPA2_LATLOG=1` (`clock_lat`: microsecond "L" lines at the right button post,
dispatch, VM handler start/end, every present, every throttle sleep of 10 ms or more, dropped timer ticks),
`NANPA2_RPROBE=1|STATE` (the exploration bot posts a right click held 9 frames every 40-160 frames and logs
the state) and `rclick` events in `NANPA2_INPUT` (now also log `RP`, keyed by their Y).
- Host path, 205 bot clicks plus 46 scripted (40.MES scene, 41.MES text): SDL/scripted post -> WM_RBUTTONDOWN
  dispatch median 0.0-0.1 ms, max 0.2 ms; -> the script handler running 0.1-0.4 ms; -> the next present
  (every 16 ms at the pump) median 10 ms, p95 17 ms; -> present after the menu block has drawn median 27 ms,
  p95 34 ms. Nothing in this path sleeps or batches.
- What waits is the original's own structure, and the C code matches it: the VM tests the buttons once per
  opcode (0x4060f9: 0x40611f left, 0x40612f right), so text (one opcode per character plus the `0x4075c6`
  frame wait) answers within a character; the wait for a click (0x406acc) reads only input 0, so a right
  click during a click wait is ignored (70 of 77 probes in that state ran no handler); a click handler
  (0x406143) first waits for both buttons to be released, so a script-level menu opens at release; a
  `04 13` menu loop (0x406671) tests inputs once per pass of its frame block, so its latency is that block's
  length. Frame-block and text lengths are the script's.
- The only host-side stall seen: the R3 throttle pays decode/dissolve charges in one `SDL_Delay`: 12-160 ms
  (median 51 ms, 38 in 3 x 9000 frames of bot play), all at picture loads, where the original's VM is also
  busy. Events wait in SDL and are read afterwards, so no click is lost and no menu is later than the load.
  Per-glyph charges (0.2 ms a byte + 0.09 ms cell) are slept in 2 ms or longer pieces, never 10 ms or more
  during text. Timer ticks dropped (more than 8 behind): 2 in 3 x 144 s.
- No pacing constants, waits or throttle changed; goldens unchanged (`li.*`, `raw.clock`). A windowed run
  could not be measured here (headless only); the Metal presenter's `nextDrawable` wait is in the status
  file (`worst_wait_ms`) for a real instance.

## 2026-11-10 (iOS / iPadOS build)

- `tools/build_ios.sh` (0f76dfc): downloads SDL2 2.32.10 into the gitignored `build-ios/`, builds it
  statically for the iOS simulator (host architecture) or device, then the app (CMake `NANPA2_IOS`, plist
  `cmake/Info.ios.plist.in`: landscape, full screen, `UIFileSharingEnabled`, `LSSupportsOpeningDocumentsInPlace`).
  `--device` uses the Xcode generator and `DEVELOPMENT_TEAM`; untested here (no signing).
- Game data (Option A): `launch_game_dir` on iOS checks `<Documents>/game` then `<Documents>`
  (`platform/docs.h`, `docs_ios.m`); otherwise a bilingual message box. Saves, `game-folder.txt` and
  `nanpa2.log` stay in the pref folder.
- Presenter: `present_metal.m` is shared; AppKit occlusion/NSApp parts are `TARGET_OS_OSX`, iOS uses
  UIApplication state and shared-storage textures (no `MTLStorageModeManaged`). `NANPA2_MACOS` keeps
  AppKit, the folder picker, `desktop_macos.c` and `bundle_macos.sh` macOS-only. macOS `--crt-shot` output
  is byte-identical before and after; ctest 8/8.
- Touch (`window.c`): SDL's touch-mouse events are off; tap = left click, hold/drag = left held, two-finger
  tap = right click, three-finger tap = CRT toggle; positions via the presenter letterbox. Background:
  music paused, pump idles (SDL_APP_WILLENTERBACKGROUND watch).
- Simulator (iPad Pro 11" M4, iOS 17.4 runtime on an Intel Mac): `Renderer: Metal | Apple iOS simulator GPU`,
  game starts from `Documents/game` and reaches the name-entry screen, CRT on and `--no-crt`; the missing-data
  message shows. Touch gestures could not be injected (simctl has no touch), so they are untested.

## 2026-11-10: display modes (CRT HD, CRT 480, Sharp, Smooth)

Commits 394553f (shaders), 7d26e4d (modes, options, preference, SDL fallback, README). Presenter only
(`src/platform/present*.{h,m,c}`, `window.c` gesture, `main.c` options).
- `present_metal.m`: new passes `fs_up` (640x480 linear picture onto the virtual raster, area-averaged
  or Catmull-Rom, half float) and `fs_hd` (the `fs_crt` tube model on that raster: beam sigma, scanline
  fade and grille by output pixels per virtual line, 8 taps when an output pixel spans more than one
  virtual pixel, wires and glow kept in tube geometry), and `fs_flat` for Sharp/Smooth (an output pixel
  wholly inside one game pixel returns that pixel's exact colour, so integer scales equal nearest).
  `fs_crt` (CRT 480) is untouched.
- `present_cycle_mode()` (F12 and the three-finger tap) cycles hd, 480, sharp, smooth; `--display`,
  `--crt-lines` (600..1600, default 1080), `--crt-scaler sharp|smooth`, env `NANPA2_DISPLAY`,
  `NANPA2_CRT_LINES`, `NANPA2_CRT_SCALER`; `--no-crt` = Sharp. Last mode in `<save folder>/display.txt`
  for plain launches only (not headless, scripted, shot, replay; `NANPA2_DISPLAY_PREF=1` forces).
- Checks: CRT 480 `--crt-shot` at 1920x1080, 3840x2160, 2732x2048, 1280x960, 800x600 is `cmp`-identical to
  master; Sharp equals nearest at 2x and 3x (`cmp`) and matches a numpy area-average reference to
  within 1 level at 2.25x, 4.27x and 3840x2160; ctest passes for the Metal and the
  `-DNANPA2_SDL_RENDERER=ON` build; `tools/build_ios.sh` (simulator) compiles.

## 2026-11-10: high-resolution text layer

Commits 784c29d (tracking, validation, glyph atlas, unit test), 57fbc1c (Metal presenter, `--text`/F11,
preference, status, README). Platform and presenter only; the one touch in `src/game/text.c` is a call
after the cell copy of 0x401900 that tells the platform where the cell went (`text_cell_placed`).
- Problem: 0x401900 copies an opaque N x N cell of 1-bit glyph (fg where the scratch pixel is set, bg
  elsewhere) into the 640x480 screen; at 1280x960 and up the 1-pixel strokes smear. Design:
  `textlayer.c` keeps a list of the cells drawn (code point, face, em, bold, the ink mask, fg/bg
  indices, screen position; at most 4096, newer cells drop the older ones they cover, `surf_fill` drops,
  `surf_blt_fast` moves entries inside the source rectangle). Per presented frame `tl_validate` compares
  the frame the presenter was given with what each glyph drew: ink = fg index, rest = bg index. A cell
  is shown if no more than 30% of its ink and 50% of its area differ (the differing pixels, e.g. the
  game's pointer, are repainted as plain pixels over the glyph); otherwise the low-res pixels show
  through. Entries are never deleted by failing validation (a restored background brings them back).
  Palette changes keep indices, so the text fades with the picture. Nothing in G, the screen surface, the
  saves or the checkpoints changes (`replay_det`, `standalone` pass).
- Measured: text is drawn to surface 0 only (the primary); no BltFast from the primary to another
  surface occurs, 4-5 primary-to-primary ones do (handled); the wipe, swap, XOR and map code write the
  screen directly (validation covers them); the cursor is drawn by raw writes (hence the repaint); the
  letterbox shifts the pointer 40 lines (the screen row is taken from the pointer). The name-entry grid,
  the title and status numbers are pictures, not 0x401900 text: they stay low-res. The ASCII cell is 16
  wide but advances 8, so a half-width cell is tracked at its advance (no partial overlaps observed:
  `overlaps 0` over the runs).
- Rendering (`present_metal.m`): the glyph is the face's outline (stb_truetype, antialiased) at the
  on-screen size, positioned where the bitmap one was (the strike's advance box centre and baseline for
  glyphs that came from a bitmap strike, the outline's baseline otherwise; +0.25 game px down measured
  with `NANPA2_LOG_TEXT=2`: box centres agree to 0.1-0.3 game px), GDI's bold as a horizontal widening
  of 0.75 game px, coverage raised to the 0.8 power to match the bitmap weight (ink area vector/bitmap
  about 0.75 before that). One opaque quad per cell, `mix(bg, fg, coverage)` in linear light: in CRT HD
  into the half-float virtual raster between `fs_up` and `fs_hd` (same beam, grille, glow), in Sharp
  and Smooth onto the drawable after `fs_flat` (gamma encoded in the shader; no blending, no extra
  full-screen pass). CRT 480 and the SDL_Renderer presenter keep the game's glyphs. Glyph cache keyed by
  (code point, face, font, em, bold, scale/64), 2048x2048 atlas, reset when full; at most 4 ms of
  rasterising per frame (entries whose glyph is not ready stay low-res that frame).
- Options: `--text hires|bitmap`, `NANPA2_TEXT_LAYER`, F11, a `text hires|bitmap` second line of
  `display.txt` (same rule as the display mode). iOS: no gesture, the option and the preference only.
  Dev: `NANPA2_SHOT_PRESENT=2` (also writes `bitmap_NNN.bmp`, the same frame without the layer),
  `NANPA2_SHOT_SIZE=WxH`, `NANPA2_LOG_TEXT=1|2`, `NANPA2_TEXT_WEIGHT`, `NANPA2_TEXT_SMEAR`; the status
  file has a `text` object.
- Checks: with `--text bitmap` the presenter output is `cmp`-identical to master in CRT HD, CRT 480,
  Sharp and Smooth for in-game frames (16 of 16) and through `--view-bmp` (16 of 16); ctest passes for the
  Metal and the `-DNANPA2_SDL_RENDERER=ON` build (`smoke` skips here: the window is not visible);
  `tests/textlayer_test.c` checks tracking, validation, repaint, blit and fill without game data.
  GPU cost of a capture at 1280x960 / 3840x2160: +1.5 / +2-4 ms with text (CRT HD 7.5 vs 6 ms and 20 vs 17.5 ms;
  these include the read-back). The glyph rasteriser takes about 0.05 ms a glyph at 2.25x.
- Known gaps: the pointer over a cell that loses more than 30% of its ink falls back to the bitmap glyph
  for that cell; the first frames of a full page at 4K can mix vector and bitmap glyphs while the atlas
  fills (4 ms budget); text moved by the game's raw copies (wipe, swap) stays bitmap until redrawn; the
  presented frame on screen could not be checked live here (window not visible), only captured through
  `present_capture`; the iOS build was not compiled.
