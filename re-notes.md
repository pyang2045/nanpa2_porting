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
| `SPEECH/A..O/*.WAV` | 10,768 voice clips, 22 kHz 8-bit mono PCM; Chinese (Mandarin) dub, confirmed by ear on A10012 only |
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
    waits for a voice to finish. Language: the dub is Chinese (Mandarin); listened to
    `A10012.WAV` only, the other clips are assumed the same.
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

Content: the Taiwan publisher's logo. It is the first of two openings; the
second is ELF's animated logo, run by `start.mes` (loads `mouse.dat`, `elf.gp4`, `elf.ani`,
then jumps to `start1.mes`, the title). 0x401e20 has one caller, WinMain 0x402166 (`push
0x431134` "LOGO.AVI"; `call 0x401e20`; `add esp, 4`; `call 0x402620`), which ignores EAX.
Its only global is 0x4ac4f8 (the play loop flag), 0 on return. Display mode, palette, sound
device and the 16 ms timer are all set up afterwards by 0x402620, so skipping the movie
leaves the game in the same state.

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

Port fonts (not game data, read from the system): Apple LiSung Light (`AppleLiSung-Light.ttf`,
a macOS download under `/System/Library/AssetsV2/com_apple_MobileAsset_Font8/<hash>.asset/
AssetData/`) has Apple `bloc`/`bdat` tables (same layout as OpenType EBLC/EBDT) with 1-bit
strikes at 9, 12, 16, 24 ppem; index subtables are format 1 with image format 6 (big
metrics, byte-aligned rows) and format 2 with image format 5 (bit-aligned, metrics in the
index). The 16 ppem strike has ascender 13, descender 3; ideographs (advance 16) use columns
1-14 and rows 0-15, so with the 1-pixel bold they fit the 16-pixel cell of 0x401900. It
covers 13457 of 13752 CP950 codes; the missing ones are kana, a few symbols and U+FF5E ～
(Big5 A1E3, frequent in the script). Songti.ttc face order: 0 SC Black, 1 SC Bold, 2 TC
Bold, 3 SC Light, 4 STSong, 5 TC Light, 6 SC Regular, 7 TC Regular; faces 1-7 cover the
Traditional-only probe characters.

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

### Cursor

The system cursor is hidden for the whole game: ShowCursor (IAT 0x41b11c) is called at
0x401e27 (0, first thing in the logo player 0x401e20, also when no movie plays, never
undone), 0x40262a (1) and 0x4028ab (0) in the DirectDraw init 0x402620, and 0x402ac6 (1) at
shutdown 0x402a20, so the display counter is -1 while the game runs. The window class
(0x402090) gets `LoadCursorA(0, IDC_ARROW)` (0x4020cd); the window procedure has no
WM_SETCURSOR, and there is no SetCursor or SetClassLong import. The game draws its own
cursor: WM_MOUSEMOVE stores x at 0x4ac064 (0x4023bf), y at 0x4ac060 (0x4023d7); 0x4012e0 saves the
32x32 area under it and draws an AND/XOR bitmap pair at 0x4be900 + 0x100 * [0x4ac514]
(mask +0, image +0x80; clipped at x 0x260, y 0x1c0); 0x401c40 shows it (0x4ac510 = 1),
0x401c60 hides it (restore 0x4011f0, 0x4ac510 = 0). The 16 ms timer 0x4028c0 flips
0x4ac514 every 30 ticks (blink) and posts a WM_MOUSEMOVE to redraw.

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
- 0x401a50: dissolve wipe, script op `19 34` (op 0x0f, 0x407204, sub-op 4 at 0x407382). 0x80
  steps (a bit mask per 16-line band, 25 bands of 640 pixels per step); each step reads the
  frame counter at its start (0x401adf) and waits until it has advanced by 2 (0x401b7b-0x401ba4),
  so the wipe takes 256 frames (4.1 s) whatever the CPU speed, as long as a step's work stays
  under 2 frames (it is about 0.22 M instructions). If button bit 0x10 or 0x20 of `0x445d8f`
  is set at a step's wait (0x401b90) a sticky flag (`[esp+0x1c]`) drops the waits for the rest
  of the wipe, which then runs at CPU speed: measured 28.3-29.5 M instructions, 69 frames (1.1
  s) at 25 MIPS. Bit 0x10 is left button / Return / Space (0x40242f, 0x402434), bit 0x20 right
  button / Esc / Insert (0x402446, 0x40244b); key-up and button-up clear them.
- 0x4028c0: palette fade, started by 0x401d30 (`0x4ac504` = 1). The tick counter `0x4ac508`
  advances every tick and ends the fade after 17 ticks; the palette is recomputed every
  `0x44628e` ticks (1, set by ext op 0x16 at 0x406f32, which then waits at 0x406f41). Measured
  16 frames from start to end probe (256 ms) for all 121 fades of a Prologue run, at any
  `--cpu-mips`.
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

## Main game scenes (OPEN1.MES, 40.MES): hotspots and the right-click menu

After the Prologue (S63.MES jumps to OPEN1.MES) scenes are point-and-click. The script's
hit test, ext op 0x23 (0x4071a4), scans 10-byte entries {code, x0, y0, x1, y1} (picture
coordinates; the picture starts at screen y 40) until code 0xffff and returns the code of the
rectangle under the mouse in S. The table lives in a script segment ([0x44533c + 4*seg] + arg)
and is built at run time; `NANPA2_HOTSPOTS=1` logs it. The scene then sets RN = code & 0xff
and RV = code >> 8 (+1), and its main loop dispatches on `if RV == v && RN == n && F[k] == s`
blocks. Hovering a hotspot shows its verb in the slot between date and weekday (verb names:
看．調查 1, 離開 2, 打．敲 3, 推．拉 4, 給 5, 拿 6, ...).
OPEN1: Yui's mouth is code 0x0101 (183,73)-(211,94); clicking it three times (flag 10 0→1→2)
ends the scene with `1e 06 40.mes`.

Right-click: a script handler (OPEN1/40.MES 0x1284, reached from the hotspot loop) runs only
if flag 2000 == 0; it sets RV = 0 and the scene shows a pop-up list at the top left of the
picture (`04 13 01 item 02 item ... 00`), e.g. OPEN1: 伸懶腰; 40.MES (own room): 到客廳去 /
待在原地 / 結束遊戲 / 儲存、讀取. In the Prologue's text menus a right-click only cancels
(S = 0). Natively the right button is input bit 0x20 (also Esc/Insert); the only other reader
is the sprite code at 0x404484 (calls 0x405597 when [0x435573] != 0xff).

40.MES 0x274a copies 30 values into flags 1978-2007 (`0b 08 7b eb ...`, value
`B[100+I]` via op 0x2b, 0x403938: segment_base[@[7]] + register + 2*index), which includes
flag 2000.

### Diary saves and the global flags (flag 2010)

Flag 2010 marks a snapshot taken by a diary save. Every save block sets it, saves and clears
it again (S5/S7/S20/S23/S33/S35/S44/S46/S59/S60.MES 0x22d7 set, 0x2374 clear; 40.MES 0x9182
set, 0x926e clear), so it is 1 in memory only between a diary load and the loaded scene's
entry. Loading from the title (START1.MES 0x1290-0x1342) first stashes the current global
state in script segment 3 (`@[7] = 3`): the time triple, `B[100+I] = F[1978+I]` for I = 0..29
(`0d 42 08 07 93 49 20 03 08 7b eb 49 20 2e 03`) and F[2030], then runs `04 21 30 ... S`. Each
save-point scene starts with `if F[2010] == 1` (S5.MES 0x2394, 40.MES 0x26a9): it copies the
stash back into flags 1978-2007 (40.MES 0x274a, S5.MES 0x2435) and F[2030], then clears
F[2010] (S5.MES 0x2477, 40.MES 0x278c). So F[1978-2007] (including 2000, the right-click
gate) are global, not per diary. The title's Main item (shown when F[2026] = 1, see below;
START1.MES 0x9a5: S = 2 clears F[2029] and F[2033] and jumps to OPEN1.MES; F[2010] is
cleared at START1.MES 0x278) and S63.MES -> OPEN1.MES both reach OPEN1 with F[2010] = 0.
OPEN1.MES has no restore block and cannot save: a FLAG file naming OPEN1.MES with F[2010] = 1
makes 40.MES restore whatever OPEN1 loaded into segment 3 (picture bytes; flag 2000 = 15).

Own room (40.MES): the bed is hotspot 0x16 (58,186)-(244,239) (picture coordinates). It
offers 睡1小時 ... 睡8小時, then 寫日記 / 看日記 / 什麼都不做; 寫日記 opens the six-book picker
(books at screen (295,82) ... (533,82), book 1 = FLAG1). Pop-up items are 18 px apart from
screen y 57. The title menu with Main shown: Prologue (320,170), Main (320,198), Data Load
(320,254); the title's diary picker has book 1 at (200,170).

### Title menu (START1.MES)

On the first title of a session (`if F[2015] == 0`, START1.MES 0x1fd) the title loads FLAG6
with ext op 0x21 mode 2 (`04 21 32 03 02 36`), which keeps the current first 32 bytes. FLAG6
holds the global state. From flags it read (406, 540, 210 with 763 = 0, 781, 779, 780) it
sets F[1978-1983] (0x2a0-0x300) and saves FLAG6 again (`04 21 31 03 02 36`, 0x306). F[2015]
is set at 0x35a. With F[2026] = 0 and no diary (A = 0) it goes straight to OPEN.MES (0x360).
The menu's item flags are set at 0x38c-0x3be:
- F0 = 1: Prologue.
- F1 = F[2026]: Main.
- F2 = 1: Data Load.
- F3 = F[1983] + ... (15 flags down from 1983): a further item.
The only script write of F[2026] is in 40.MES's diary-save block (0x91cc, before the FLAG6
save at 0x9260). Main therefore appears once the player has written a diary in the main game.
Returning to the title in the same session also sets it (0x280: F[2015] = 1 -> F[2026] = 1).
Choosing Main (S = 2, 0x9bf) clears F[2029] and F[2033] and jumps to OPEN1.MES.

### Calling a MES file (ext ops 0x1e, 0x1f)

`04 1e` (0x406f7c) jumps to another MES file: it loads it over the script buffer and restarts
the VM at the file's entry with the native stack reset (`esp = [0x445cdc]`). `04 1f`
(0x406fcd) calls one as a subroutine (42.MES calls TOMOMI.MES, the town calls TIME.MES, the
quiz calls QUIZ_SUB.MES): it pushes the script offset, copies the label table (0x445dbc,
0x1e dwords) to 0x445e36 and the file name (0x444e78, 8 dwords) to 0x445d46, saves the
handler state (0x445ce1/ce2/ce4), loads the file and runs it with a nested `call 0x4060f9`.
The sub-file returns with op 00/02 (0x4061aa, `ret` when esp is not the top level). Then
the caller restores everything with `mov cx, 0x1e` / `mov cx, 0x8` + `rep movsd`
(0x40705e, 0x40706e) and reloads its own file. Those count loads set only CX: the native
code works because the last thing to run before them is the message pump (0x401bf0, called
at every opcode fetch, 0x4060f9), whose PeekMessageA/DispatchMessageA leave ECX changed.
A replacement pump has to clobber ECX (to a value with a zero high half) or the copy runs off
memory.

## Running the game code: interpreter, recompile, lockstep

The game code the engine runs is 0x401000-0x407800 (the MSVC runtime after it is replaced
by address in src/hle/crt.c). It is 6045 reachable instructions (traversal from WinMain
0x402090, WndProc 0x4021c0, the logo subclass proc 0x401fd0, the jump tables listed in
tools/recomp.py and the code pointers in .data 0x435576-0x4355fa, 0x435dfc-0x435e60).

- Instruction set actually used (opcode bytes, counts in the reachable code): ALU 00-3d in all
  forms, 80/81/83 groups (add or adc and sub xor cmp), 84/85 test, 86 xchg, 88-8b mov, 8d lea,
  40-4f inc/dec, 50-5f push/pop, 60/61 pushad/popad, 68/6a push imm, 0faf/f7 imul, f7 mul/div,
  c0/c1/d0/d1 shifts and rotates (rol 6, rcl 1, rcr 2, sar 1), f6/f7 test/not, fe/ff
  inc/dec/call/jmp/push, 0fb6/0fb7 movzx (no movsx), 0f95 setne, 0fc8 bswap, 98/99 (cwd only,
  with 66), a0-a3 moffs, string ops (rep movsb/movsd, rep stosw/stosd, repne scasb, lodsd,
  movsb, movsd, stosd; cld/std once each), e2 loop (24), e3 jecxz (2), 70-7f/0f8x jcc, e8/e9/eb,
  c2/c3. Prefixes: 0x66 on 781 instructions (16-bit arithmetic, moffs16, `66 c7` with imm16),
  f3 30, f2 1; no 0x67, no segment overrides, no x87 (the one x87 routine, the timer
  callback 0x4028c0, is an override). Indirect calls are `call [reg+disp]` (COM methods),
  `call [IAT]` and `call [eax*4+table]`; none uses ESP as base, so the order of the target
  read and the return-address push does not matter here.
- Overrides and probes are listed once, in src/hle/overrides.h (X-macro). The interpreter
  marks override addresses in a per-byte table over the code range; entering one, or any
  address outside the range (import/COM thunks, CRT), calls hle_call with the return address
  on the stack, as the recompiled dispatch does.
- Semantics shared by both backends: flags from the x86.h helpers; a poll (X86_POLL) on every
  taken direct branch (jmp, jcc, loop, jecxz) to game code at or before the branch address;
  one instruction count per instruction plus one per repeated string iteration (the recompile
  adds a block's count at its start, so the totals agree at every block boundary); the
  replaced pump 0x401bf0 clears ECX and EDX in both.
- Lockstep (recomp build, NANPA2_LOCKSTEP=1): tools/recomp.py starts every block with
  `X86_BLOCK(address, instructions)`. There the interpreter runs those instructions first, on
  a copy of the registers, its writes journalled (a bit per byte of emulated memory, reads see
  the journal), without polls, probes or HLE; then the recompiled block runs with its writes
  logged (X86_WTRACK, compiled into recomp.c only). The two are compared at the next block
  start, at hle_call entry (before the HLE touches anything), at x86_poll entry (before the
  timer callback writes 0x446292 and the palette) and at the return to the host. A host call
  in the middle of a block (a probe running game code through x86_call, as the exploration
  bot does with 0x4037fc) drops that block's comparison.
- nanpa2.exe is recognised by its PE TimeDateStamp 0x39697c6a (2000-07-10 07:34:02 UTC) and
  SizeOfImage 0xc1000; it is not on the CD as a file but inside `data1.cab`, an InstallShield
  5 cabinet (`ISc(` magic, version 0x01005100, 114486 bytes for nanpa2.exe and FLAG0..6).
- Oracle entries (HLE_GAME_OVERRIDE, reimplemented game logic): x86_exec(X86_ORACLE_TAG |
  addr) runs the original body. Recompile: a `case` on the tagged address into the lifted
  body. Interpreter: the override check is skipped for the first fetch of addr, and again
  after any direct jump (jmp, jcc, loop) to addr, which recomp.py allows only from inside
  that body (check_oracles); calls and indirect transfers to addr still reach the override.
