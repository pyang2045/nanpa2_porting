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
