# nanpa2.exe reverse-engineering notes

Taiwan Traditional Chinese release of 同級生２, `nanpa2.exe` (PE32, linked 2000-07-10,
MSVC runtime statically linked, not packed). Imports only DDRAW (DirectDrawCreate),
DSOUND (DirectSoundCreate), WINMM (timeSetEvent/timeKillEvent/mciSendStringA), USER32,
GDI32 (CreateFontA/TextOutA) and KERNEL32.

## Disc layout

| Path | Contents |
|---|---|
| `DATA/*.GP4` (462) | pictures, compressed (below) |
| `DATA/*.ANI` (243) | animations, compressed with the same codec |
| `DATA/*.MES` (470) | scripts: bytecode with inline Big5 text |
| `DATA/*.A6` (339) | small u16 tables (hit areas?) |
| `DATA/*.MAP`, `*.C5` (7 each) | town/school maps: GAKKO1-4, MATI1-2, SENRO |
| `DATA/MOUSE.DAT` | cursor bitmaps, 8192 bytes |
| `MUSIC/NN.mp3`, `NN.wav` | BGM, played through MCI (`MUSIC\%02d.MP3`, `.WAV`) |
| `SPEECH/A..O/*.WAV` | 10,768 voice clips, 22 kHz 8-bit mono PCM |
| `LOGO.AVI` | opening logo, Cinepak 640x480 + PCM, MCI `avivideo` |
| `data1.cab` | InstallShield 5: `nanpa2.exe`, `FLAG0`..`FLAG6` (installed to the HD) |

`FLAG0` starts with the string `start.mes`.

## Startup (0x402090)

- Window class/title `Nanpa2`, full-screen popup.
- 0x402590: CD search. For drive letters A..Z with GetDriveType == CDROM, open
  `X:\SPEECH\A\A10930.WAV`; the first hit sets the path prefix `DAT_00431030` ("X:\").
  Falls back to the current directory. Failure: MessageBox 請插入同級生２光碟片.
- 0x401e20: plays `LOGO.AVI` through MCI.
- 0x402620: DirectSound init (0x402ad0: primary buffer PCM 22050 Hz, 8-bit, stereo
  (block align 2)), DirectDraw 640x480x8 exclusive, three 640x400 offscreen surfaces
  (`DAT_004462a2`..) plus a 640x480 one (`DAT_004ab058`), fonts 細明體 (MingLiU, charset
  0x88 = CHINESEBIG5) at heights 15/24/32 (weights 700/700/400), a 16 ms multimedia timer
  (0x4028c0).
- 0x4060d4: loads `start.mes` and runs the game (0x402a20 is shutdown).

## Screen

- 8-bit palette, 640x480. Surfaces are indexed by `DAT_0044629e[n]` with pitches
  `DAT_004462ae[n]`; [0] is the primary.
- Game content is 640x400; with `DAT_004ac068` set, everything drawn to the primary is
  offset 40 lines down (0x401430, 0x4014f0).
- Palette index 8 is transparent in masked blits (0x4014f0).
- Palette (0x401ca0): entries 0-15 come from 16 u16 at `DAT_0044530c` as 0x0GRB 4-bit
  (R = v&0xf0, G = (v>>4)&0xf0, B = (v<<4)&0xf0); entries 16-255 from 240 u16 at
  `DAT_00445eae` as RGB555 (R = (v>>7)&0xf8, G = (v>>2)&0xf8, B = v<<3). 0x40752c copies a
  picture's 512-byte palette into those two tables.
- Text (0x401900): one Big5 or ASCII character at a time through GDI TextOutA into the
  scratch surface, then copied into the primary with foreground/background indices.

## File loading (0x401050)

`name` -> `DATA\name`, except names starting with `FLAG`, which open as given (the install
folder). Each open first tries the path as given, then with the CD prefix (0x401000).
`.GP4` and `.ANI` are decompressed (0x4075f0); other files are read raw.

## GP4/ANI codec (0x4075f0)

LZHUF (Okumura/Yoshizaki adaptive Huffman + LZSS) with changes:

- bits read LSB-first (`bt [esi], ecx`);
- N_CHAR 321 (256 literals + lengths 3..67 as code - 253), T = 641 nodes, root 640; the
  initial tree is the standard StartHuff (working copy 0x4a864c, pristine copy 0x4a74c0);
- match position: the next 8 stream bits index d_code (0x4a72c0) and d_len (0x4a73c0) for
  the upper 6 bits and the prefix length, then 6 raw low bits; distance = position + 1;
- no ring buffer, matches copy from the output; position 0xfff ends the stream;
- rebuild at root frequency 0x8000 (0x40767c): for each leaf whose parent's count is odd run
  update() from that parent, then halve every count;
- the root's parent entry is 0 (update stops there).

Port: `src/lzh.c`; reference: `tools/lzh.py`.

## GP4 picture

Decompressed: u16 x, y, w, h; u16 palette[256] as above (16 x 0x0GRB + 240 x RGB555);
u8 pixels[w*h]. Backgrounds are 512x288 at (64,0); full screens 640x400 at (0,0).

## Input (WndProc 0x4021c0)

Button/key state byte `0x445d8f`, set on WM_KEYDOWN/WM_*BUTTONDOWN and cleared on the
matching up message (key tables: VK-0x0d indexes bytes at 0x4024a4 / 0x40252c into jump
tables 0x40247c / 0x402508):

| bit | source |
|---|---|
| 0x01 / 0x02 / 0x04 / 0x08 | Up / Down / Left / Right |
| 0x10 | left button, Enter, Space |
| 0x20 | right button, Esc, Insert |
| 0x40 | Ctrl (skip) |
| 0x80 | End |

F1 (0x4022dd) calls an import with `0x4ab05c`. Mouse position: `0x4452e2`/`0x4452e4`.
0x40609c returns bl = bit0 (0x10) | bit1 (0x20), cx/dx = mouse x/y.

## Pacing (what is timed by what)

Everything visible is paced by the 16 ms multimedia timer (frame counter `0x446292`,
incremented in 0x4028c0); nothing in the game code uses timeGetTime/GetTickCount, and the
only WaitForVerticalBlank is in WM_PAINT (0x402570).

- 0x4075c6: wait ecx/4+1 frames, pumping messages (0x401bf0) and stepping animations
  (0x403624) meanwhile. Used by the script wait opcode (0x406ab8), the per-character text
  delay (0x406310 / 0x40637e, ecx = text speed `0x4452de`, script variable 8 of the array at
  `0x4452ce`; 0 = no delay, as in the Prologue), and the
  wait-for-click loop 0x406acc (one frame per poll).
- 0x403624: animation tick, at most once per frame (`0x4313bc` = last frame seen); steps the
  ten animation slots at `0x4312cc` (stride 0x18) through 0x403661. 0x4035a0 waits frame by
  frame until one slot finishes.
- 0x401a50: dissolve wipe, 0x80 steps (a bit mask per 16-line band), each step waits 2 frames
  (about 4 s); with a button (0x30) held it stops waiting and runs at CPU speed (about 16 M
  instructions, 0.65 s at 25 MIPS).
- 0x4028c0: palette fade, one step every `0x44628e` ticks, 16 steps.
- Script opcode at 0x4072c3/0x4072f2: frame-counter stopwatch (`0x446296`).

CPU-bound: only the work between waits (script interpretation, picture decompression
0x4075f0, blits 0x401430/0x4014f0, text 0x401900). The port throttles it to an emulated
instruction rate (`--cpu-mips`, default 25). Measured instruction counts of the lifted native
decompressor: ELF.GP4 (6 KB in, 256 KB out) 0.71 M, NAME.GP4 (32 KB in) 2.45 M,
02.GP4 (15 KB in, 147 KB out) 1.28 M, ELF.ANI (28 KB) 2.32 M: about 70 instructions per
input byte plus 1.2 per output byte.

## Save files

0x401160 save(name, buf, size): fopen(name, "wb") + fwrite + fclose. The script save opcode
(0x407188) writes 0xc00 bytes from the script buffer `0x444e78` to the name at `0x445d86`
(FLAGn); load (0x407152) reads it back through 0x401050.
