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
  piece replays on reactivation, as a stopped one-shot DirectSound buffer is back at 0).
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
