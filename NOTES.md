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
