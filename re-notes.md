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

## Sound (0x402ad0-0x4034a0)

DirectSound only; MCI (`mciSendStringA`) is used for nothing but `LOGO.AVI` (0x401e20). No
buffer ever gets SetVolume/SetPan/SetFrequency (vtable calls used: 0x0c CreateSoundBuffer,
0x18 SetCooperativeLevel, 0x24 GetStatus, 0x2c Lock, 0x30 Play, 0x34 SetCurrentPosition,
0x38 SetFormat, 0x48 Stop, 0x4c Unlock, 0x50 Restore), and there is no volume setting.

- 0x402ad0 init: DirectSoundCreate, priority level, primary buffer set to PCM 22050 Hz 8-bit
  stereo. Failure only shows a MessageBox; the caller 0x402620 ignores the result.
- Two secondary buffers, nothing else:
  - music: buffer `0x4be560`, file image `0x4be570`, `0x4be54c` = 1 for WAV. 0x402cb0(n):
    stop music (0x4032c0), then `MUSIC\%02d.MP3` (0x4312a8), else `MUSIC\%02d.WAV`
    (0x431298), read whole (0x401000 open with the CD prefix). MP3: decoder 0x402e10
    (state `0x4be56c`), 0x4030f0 makes a 0x24000-byte stream buffer (44100 Hz 16-bit stereo,
    caps 0x181e0 with position notify), 0x402ea0 sets two notifications (events
    `0x4be550`/`0x4be554`, offsets 0 and 0x12000), Play with DSBPLAY_LOOPING. The message
    pump 0x401bf0 calls 0x4034a0, which waits on the events and refills the played half
    (0x4031d0 decodes 0x12000 bytes through 0x409550; on -0x202, end of stream, it rewinds
    with 0x4084b0(dec, 0)): MP3 music loops forever. WAV: 0x403020 copies the data chunk
    (0x402f10 finds `data` after the fmt chunk) into a static buffer (caps 0x180e2) in the
    file's own format, Play(0,0,0): plays once.
  - voice: buffer `0x4be564`, file image `0x4be574`. 0x403360(name): stop voice (0x403440),
    `SPEECH%s` (0x4312c0, name from the script, e.g. `\A\A10012.WAV`), 0x403020,
    SetCurrentPosition(0), Play once. Nothing ever polls the voice status, so the script never
    waits for a voice to finish.
- Script music opcode (0x4073a5): arguments parsed by 0x40741b into words at `0x445d06`...;
  first argument 0 stops the music, otherwise music number = second argument (`0x445d08`,
  zero-extended).
  Voice opcode 0x4073cd copies the name (0x406dc4) to `0x445d26` and calls 0x403360.
- The `MUSIC\4x-5x.WAV` files (22050 Hz 8-bit mono, 0.3-17 s) are jingles and effects; they go
  through the music channel and replace the BGM. MP3s are all 44100 Hz stereo 96 kbit/s.
- WndProc (0x4021c0) stops the voice on WM_LBUTTONDOWN/WM_RBUTTONDOWN (jump table 0x402550:
  0x40242f, 0x402446), on WM_KEYUP of Return/Space (0x402361) and Esc (0x402378) (tables
  0x402508/0x40252c), and on WM_ACTIVATEAPP(0) (0x402235: voice stop, then 0x403220 Stop the
  music if playing). WM_ACTIVATEAPP(1) after a deactivation (`0x4ac518`) calls 0x403250:
  Play again, looping for MP3, once for WAV (restoring a lost WAV buffer).
- 0x402c70 (exit): voice stop, music stop (0x4032c0 also frees the decoder 0x409f40 and the
  notify object), release primary buffer and DirectSound.

Port: `src/hle/sound.c` (SDL audio, minimp3); 0x402f10/0x402f60/0x4031d0 are only reached
from the replaced functions and are stubs in `override_call`.

## LOGO.AVI (0x401e20, window procedure 0x401fd0)

RIFF AVI, 451 frames at 30 fps (33333 us), two streams: `vids`/`cvid` (Cinepak) 640x480
24-bit, 31 key frames in `00db` chunks and 420 inter frames in `00dc`, 3 strips per frame;
`auds` PCM 8-bit mono 22050 Hz in 323 `01wb` chunks (330750 samples, 15 s). 0x401e20 hides
the cursor, sets DirectDraw to 640x480x16, then MCI: `open avivideo!%s alias vfw` (retried
with the CD prefix), `window vfw handle %d`, `put vfw destination at 0 0 640 480`, `play vfw
window from 0 notify`, and pumps messages through 0x401fd0 until 0x4ac4f8 is cleared:
MM_MCINOTIFY (0x3b9) with wParam 1, or WM_KEYDOWN / WM_LBUTTONDOWN / WM_RBUTTONDOWN
(skip). WM_ACTIVATEAPP sends the strings at 0x431128 (active) / 0x43111c (inactive).
Afterwards `stop vfw wait`, `close vfw wait`, the old window procedure and full-screen size
are restored. Port: `src/cinepak.c` (decoder), `src/hle/logo.c` (player).

## Text routine (0x401900)

cdecl (char, x, y, size, fg, bg); called from the script VM at 0x4062ff (ASCII, size 0) and
0x40636d (Big5, size from 0x4452f6) with x = column 0x4452f0 * 8, y = 0x4452f2, fg/bg the
low/high nibble of 0x4452f4. A char with bit 15 set is Big5 (high byte first). It selects
font 0x4ac070[size & 3] into the scratch surface 0x4ab058's DC, `SetTextColor(0xf0f0f0)`,
`SetBkColor(0)`, `SetBkMode(OPAQUE)`, `TextOutA(dc, 0, 0, s, 1 or 2)`, then copies an N x N
cell with N = 8 * size + 16 to primary + pitch * y + x, writing fg where the scratch byte is
non-zero and bg elsewhere (so the cell is opaque), clearing each scratch row (32 bytes,
pitch 640) as it goes. No shadow. The fonts (0x402620, lines CreateFontA) are MingLiU with
negative heights, i.e. em heights 15/24/32, weights 700/700/400, charset CHINESEBIG5, so
cells are 16/24/32 for ems 15/24/32. ASCII uses the same N x N cell at 8-pixel columns, so a
glyph wider than 8 pixels is overwritten by the next character's cell.

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

The map/drawing range 0x404132-0x405fb6 has no frame-counter reads (the only readers of
`0x446292` in the game code are at 0x401adf/0x401b7b/0x401b99 (wipe), 0x4029bb/0x4029c9 (fade),
0x4035b5/0x40362d (animation), 0x4072c2/0x4072f2 (stopwatch) and 0x4075ca/0x4075e5 (frame
wait)). 0x405b07 (recursive, called about 25 times from 0x405c88-0x405f8b) and the
0x404731/0x404937/0x404a9a/0x404c9f group under 0x405691 are pure computation between waits, so
the throttle covers them and nothing there needs a timer.

## Save files

0x401160 save(name, buf, size): fopen(name, "wb") + fwrite + fclose. The script save opcode
(0x407188) writes 0xc00 bytes from the script buffer `0x444e78` to the name at `0x445d86`
(FLAGn); load (0x407152) reads it back through 0x401050.

Script form: `04 21 mode 03 02 slot 03`, mode 0x31 save / 0x30 load / 0x32 (START1.MES 0x23d,
reads FLAG6), slot an expression. The 存檔 item in a scene (S5.MES 0x235a/0x2367; the same block is
at OPEN.MES 0x2522) saves slot 0 and then slot 6; FLAG6 is the diary index the title reads
(START1.MES, its "Data Load" item lights the used diaries). The title load is
`04 21 30 03 02 53 03` (START1.MES 0x1342): slot = register S from the diary picker
(請選擇日記, six books in a row at y 170, the lit one at x 440 for slot 0; Exit at (410,204)).
Title menu: Prologue (320,170), Data Load (320,226).

Menus return the item's index in the list as written, not its position on screen: hidden
items (a false `0f expr 03` guard) are packed out of the 2x2 grid ((215,383) (420,383)
(215,407) (420,407)), so S5's 存檔 is item 4 even when it is drawn third. A click outside every
item takes the highlighted item.

## Script expressions and menus (0x4037fc)

RPN on the native stack, ended by 0x03. Tokens: 0x30-0x3f literal 0-15; `07 b` literal b>>2;
`08 h l` literal ((h>>2)<<6 | l>>2); 0x40-0x5a register A-Z (u16 at `0x445298 + 2*n`);
0x20 + 0x21 - (saturating at 0) 0x22 * 0x23 / 0x24 % 0x25 | 0x26 & 0x27 == 0x28 != 0x29 <
(a b -> 1 if b < a... pops top first), 0x2d n: 4-bit flag n (nibbles at `0x444e98`, odd n in the
high nibble, 0x4039d6), 0x2f n: random (0x403999). Statements: `0f expr 01 ... 00` if-block
(0x40659e; 0x4061d4 skips a block by counting 01/00 nesting), `0a n expr` set flag n
(0x406491, clamps to 15, 0x406455), `0c R expr` set register (0x4064d9), 0x11 wait for a
click (0x4065f6 -> 0x406acc), 0x13 call, 0x04 xx extended opcodes (table 0x406262, xx-0x10).
Menus: item lists `02 [0f expr 03] text` ... `00`; the choice comes back in register S
(1-based, 0 = cancelled). OPEN.MES (Prologue) keeps the main choice in V and the sub
choice in N; the main loop starts at script offset 0x2a3c.
