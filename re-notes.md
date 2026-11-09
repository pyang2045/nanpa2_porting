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
  first argument 0 plays music number = second argument (`0x445d08`, zero-extended,
  0x4073b9 -> 0x402cb0), any other first argument stops the music (0x4073af -> 0x4032c0)
  (corrected 2026-11-08: `cmp eax, 0; je 0x4073b9` at 0x4073aa).
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

Exact behaviour (reimplemented in src/game/text.c): the string is built in ECX's save slot
(the char's bytes over ECX's low bytes), so ECX returns as that dword; the DC comes from the
scratch surface's GetDC (vtable +0x44) and goes back through ReleaseDC (+0x68); the cell copy
locks the scratch surface, then the screen (0x401190, which hides the cursor on the
screen), and unlocks them in the same order (0x4012b0, which redraws it), so EDX returns as
the scratch surface object unless the cursor redraw changes it. The window and cursor,
u16 each unless noted: 0x4452e8/ea/ec/ee left, top, right, bottom (x in 8-pixel columns, y
in pixels), 0x4452f0/f2 cursor x, y, u8 0x4452f8 line height, u8 0x4452f9 character width
in columns, u8 0x4452f6 the Big5 size, 0x4452de (@[8]) the delay after each character
(0x4075c6 with ECX = it, so v/4 + 1 frames; 0 none). 0x4063ee advances x by the width and
takes a new line when x + width - 2 >= right; the newline 0x406415 (also Big5 A1BE) sets
x = left, y += height, and y = top when y + height - 2 >= bottom. 0x406b24 fills the window
on surface @[6] with colour EBX (0x4017b0) and homes the cursor.

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
`08 h l` literal ((h>>2)<<6 | l>>2); 0x40-0x5a register `@`-`Z` (u16 at `0x445298 + 2*n`,
n = token - 0x40; exact semantics below);
0x20 + 0x21 - (saturating at 0) 0x22 * 0x23 / 0x24 % 0x25 | 0x26 & 0x27 == 0x28 != 0x29 <
(a b -> 1 if b < a... pops top first), 0x2d n: 4-bit flag n (nibbles at `0x444e98`, odd n in the
high nibble, 0x4039d6), 0x2f n: random (0x403999). Statements: `0f expr 01 ... 00` if-block
(0x40659e; 0x4061d4 skips a block by counting 01/00 nesting), `0a n expr` set flag n
(0x406491, clamps to 15, 0x406455), `0c R expr` set register (0x4064d9), 0x11 wait for a
click (0x4065f6 -> 0x406acc), 0x13 call, 0x04 xx extended opcodes (table 0x406262, xx-0x10).
Menus: item lists `02 [0f expr 03] text` ... `00`; the choice comes back in register S
(1-based, 0 = cancelled). OPEN.MES (Prologue) keeps the main choice in V and the sub
choice in N; the main loop starts at script offset 0x2a3c.

### Script VM state

Set up by 0x406395 (called before every MES file runs):

| Address | Type | Meaning |
|---|---|---|
| `0x435e78` | bytes | script image; the VM pc (ESI) is an offset from here; segment 0 |
| `0x444e78` | char[32] | name of the running MES file |
| `0x444e98` | nibbles | flags: flag n in byte n/2, low nibble for even n, high for odd |
| `0x445298` | u16[27] | registers `@` (0x40) to `Z` (0x5a); token 0x40 + i is register i |
| `0x4452ce` | u16[] | word array; register `@` = its segment-0 offset (0x4452ce - 0x435e78) |
| `0x4452dc` | u16 | `@[7]`: data segment used by array reads and writes (0x2b/0x2c, ops 0x0d/0x0e) |
| `0x44533c` | u32[4] | segment bases: 0x435e78, 0x4462be, 0x4562be, 0x4562be |
| `0x4313c4` | u16 | random seed |

So the register letters are one more than an index from `A`: `S` (0x53) is register 0x13.

### The VM loop (0x4060d4, reimplemented in src/game/loop.c)

0x4060d4: push ebp, [0x445cdc] = ESP (the top-level frame), VM init 0x406395, load the MES
file named at 0x444e78 into the script image, ESI = its first word. Then each pass at
0x4060f9: pump 0x401bf0 (0: WM_QUIT, `mov esp, [0x445cdc]; pop ebp; ret` at 0x40638d),
the animation step 0x403624, and unless bit 2 of @[9] (0x4452e0) or a click handler is
running (0x445ce1), the button tests 0x4074d5 (AL 0 then 1): a button down whose handler
(0x445ce2/ce4, set by ext 0x20) is set waits for the release and runs the handler's block
(0x40613f-0x40616c); a button down without one skips straight to the fetch. The fetch
0x40616e: 00 and 02 end the block (`inc esi`; at the top level, ESP = [0x445cdc], the VM
ends; else `ret` with AL = 0 and EAX's upper bytes ESP's); 01 pushes ESI, runs the block
(0x4061cd: `inc esi; call 0x4060f9`), pops ESI back to the 01, skips the block with
0x4061d4 and loops; 04 xx indexes table 0x406262 with xx - 0x10; 06 starts ASCII text
(each byte drawn at size 0 until 06); a byte from 0x80 and the next are one Big5
character; bytes below 9 and from 0x20 end the VM; 09-1f index table 0x40620b with op -
0x0a (09 and 1b-1f fall outside it). The input test 0x4074d5 pumps (WM_QUIT ends the VM)
and tests [0x445d8f] against 0x10, 0x20, 1, 2, 4, 8 for AL 0-5 (ZF set: up). The click wait
0x406acc returns as soon as Ctrl is down (bit 0x40, tested each round), else waits frame by frame (0x4075c6,
ECX = 1) for input 0 down and then up.

The click handler path in detail (0x40613f-0x40616c, stage 6). A handler offset is a u16 at
0x445ce2 (left) / 0x445ce4 (right), 0 = none; `04 20 button, offset` (0x4070bc: parse,
`[0x445ce2 + 2 * button] = arg 1`) sets one. **No shipped script uses `04 20`** (nor `04 1a`;
tools/mes_ops.py counts the byte pairs `04 xx` in all 470 MES files: zero hits), so the path runs
only with a modified script or the dev hook NANPA2_CLICKTEST (src/hle/dev.c writes a block `01 10
00` at the end of the script image and points both handlers at it). With a handler set and its
button down (ZF clear after 0x4074d5): wait until the left button, then the right button, are
both up (0x406143: `mov al, 0; call 0x4074d5; jne 0x406143; mov al, 1; call 0x4074d5; jne
0x406143`), then `push esi; [0x445ce1] = 0xff; esi = handler; call 0x4061cd` (run the block),
`[0x445ce1] = 0; pop esi; jmp 0x4060f9`. The busy flag keeps the handlers from nesting.

### Script expressions: exact semantics (0x4037fc, reimplemented in src/game/expr.c)

The evaluator is called (`call 0x4037fc`) with ESI = script offset of the first token and
returns EAX = value, ESI past the 0x03. It pushes 32-bit values on the native stack above its
return address, and 0x03 is `pop eax; ret`. Values are 32-bit unsigned throughout.

Operands (each pushes one value):

| Token | Value |
|---|---|
| `30`-`3f` | 0-15 (0x403a06) |
| `07 b` | b >> 2 (0x403a12) |
| `08 h l` | (h>>2) << 6 \| l>>2 (0x403a22) |
| `09 h m l` | ((h>>2) << 12 \| (m>>2) << 6 \| l>>2) & 0xffff (0x403a42; built in BX through `shr ax` / `rcr bl` twice, so the top two bits are lost) |
| `40`-`5a` | register (token - 0x40), zero-extended |

Each byte of a constant carries 6 bits in its upper bits. Operators (a = the value below the
top, b = the top; both popped, the result pushed):

| Token | Handler | Result |
|---|---|---|
| `20` | 0x4038ae | a + b (32-bit wrap) |
| `21` | 0x4038b8 | a - b, 0 if b > a |
| `22` | 0x4038c6 | a * b (low 32 bits, `mul`) |
| `23` / `24` | 0x4038d0 / 0x4038dc | a / b, a % b (`div ebx`, EDX = 0; b = 0 is a divide error) |
| `25` / `26` | 0x4038e8 / 0x4038f2 | a \| b, a & b |
| `27` / `28` | 0x4038fc / 0x40390b | a == b, a != b (1 or 0) |
| `29` / `2a` | 0x40391a / 0x403929 | a > b, a < b (unsigned) |
| `2b` | 0x403938 | word at segment_base[s] + a + 2b; s = 0 if (a & 0xffff) == register `@`, else `@[7]`; the result keeps the upper half of 2b (16-bit load into AX after `add eax, eax`) |
| `2c` | 0x403961 | byte at segment_base[`@[7]`] + a + b (no `@` check) |
| `2d n` | 0x40397d | flag n, n a constant operand (read by 0x4039f7) |
| `2e` | 0x40398d | flag b (pops one value) |
| `2f n` | 0x403999 | seed = (5 * seed + 1) & 0xffff, then seed % n as 16-bit `div bx` (n = 0: divide error) |

Any other token (0x00-0x02, 0x04-0x06, 0x0a-0x1f, 0x5b-0xff) ends at 0x4038ad with a bare
`ret`: with nothing pushed that returns normally with AL = the token and EAX's upper bytes
unchanged; with values pushed it returns into a value (crash). 0x03 with no value or more
than one returns into garbage the same way. For `2d`/`2f` a non-constant operand byte is used
as the number itself (0x4039f7's fall-through, EAX = that byte).

Constant reader 0x4039f7 (callers: the evaluator, and op 0x0a at 0x406491): one constant
operand as above into EAX; for `09` BX also holds the value; any other byte is returned in AL
with EAX's upper bytes kept, ESI past it. Flag read 0x4039d6: EAX = flag EAX (EBX = n>>1);
only the evaluator calls it. Flag write 0x406455: EBX = n, AX = value; values >= 0x10 (as
16-bit) store 15; EBX is preserved, AX ends clamped and for odd n AL = AL << 4.

Register contract of 0x4037fc, from its twelve call sites (0x406499, 0x4064b5, 0x4064bd,
0x4064e5, 0x406522, 0x40652d, 0x40656b, 0x406574, 0x40659e, 0x40660e, 0x4068f9, 0x40747c):
all save EBX around the call or overwrite it, reload EDX before use and read no flags, so
EBX, EDX and the flags are scratch; ECX, EDI, EBP are untouched. The full EAX matters
(0x406528 `add eax, eax` after the call). The native loop costs about 22 instructions per
token (measured: 247 per call against 11 tokens per call on average).

### Script VM helpers (reimplemented in src/game/vm.c)

More VM state, used by the helpers and handlers below:

| Address | Type | Meaning |
|---|---|---|
| `0x445cf6` | u8[16] | argument types of the last list: 1 expression, 2 string, 3 block (12 bytes cleared per list) |
| `0x445d06` | u16[16] | argument values: the value, or for a string/block the script offset of its 06/01 |
| `0x445dbc` | u16[] | labels (op 12 sets, op 13 calls) |
| `0x445ce2` | u16[2] | click handler blocks, left and right (ext 20) |
| `0x4452f4` | u16 | text colour (op 18) |
| `0x44629a` | u32 | segment index op 0d stores through |
| `0x4452fa` | u16 | `@[22]`: menu list pointer, set to 0x445d90 (as an offset) by 0x406395 |
| `0x445396` | - | buffer whose offset 0x406395 puts in `@[0]` |

- Block skip 0x4061d4: ESI in/out. Scans forward to the first 01, then counts 01 up and 00
  down in AH (8 bits, wrapping) until the depth is 0; ESI ends past the closing 00, AX = 0.
  02 inside a block is not special. Flags dead at all six call sites.
- Argument list 0x40741b (with 0x4074a3): clears the 12 type bytes, then per argument
  writes its type and value to entry n with no bound check (past 16 arguments the types run
  into the values):
  - `06 ... 06` a string: value = offset of the opening 06; AL ends as the closing 06;
  - `01 ... 00` a block, skipped with 0x4061d4: value = offset of the 01, AX = 0 after;
  - a byte that starts an expression (0x4074a3: 07, 08, 09 or 0x20-0x5a): value = the
    expression's value (16 bits stored, EAX the full value);
  - any other byte ends the list with AL = 0xff over the last EAX, ESI on that byte.
  After an argument, a 02 continues the list; anything else ends it with EAX = argument 0's
  value as a u16. Out: ESI, EAX, ECX = count, EBX = 0x445cf6 + count, EDI = 0x445d06 +
  2*count (callers read EBX and EDI). EDX is the evaluator's scratch.
- String argument 0x406dc4: EDI = destination. Type 1 (expression): argument 0 is the offset
  of a NUL-terminated string, copied with its NUL, EDI past it, AL = 0. Otherwise (a string
  literal): copied from after the opening 06 up to the closing one, then a NUL; EDI on the
  NUL, AL = 06. EBX = script offset just past the last byte read. ECX untouched (callers
  keep it).
- VM init 0x406395: `@` = 0x4452ce - 0x435e78, segments {0x435e78, 0x4462be, 0x4562be,
  0x4562be}, `@[0]` = offset of 0x445396, `@[22]` = offset of 0x445d90. EAX scratch.
- Menu item 0x4068e1 (ESI at an item of `02 [0f expr] text ... 02 ... 00`): at a 00, AL =
  0xff and nothing else changes. Otherwise it skips one byte (the 02; not checked), AL = 1,
  and with a `0f` guard EAX = the guard's value (or AL = a bare token); EBX = offset of the
  text, ESI = the next 00 or 02. Flags dead after both call sites.

### Script opcodes (reimplemented in src/game/ops.c, ops_sys.c, ops_vm.c)

Handlers are entered by `jmp` from the fetch loop with ESI past the opcode byte(s) and end
with `jmp 0x4060f9`. At 0x4060f9 only ESP, EBP and ESI are live (tools/liveness.py; the
pump 0x401bf0 that runs before the next fetch sets EAX and clears ECX and EDX), so a handler
may leave any value in the other registers and the flags. Every `jmp` back to the loop is
backward and polls; each loop over a 02 list polls at its own backward jump.

| Op | Handler | Exact behaviour |
|---|---|---|
| 0a | 0x406491 | n = constant operand (0x4039f7; any other byte is n itself); then flag n++ = expr for each expression in a 02 list (0x406455 clamps to 15) |
| 0b | 0x4064b5 | as 0a with n an expression |
| 0c | 0x4064d9 | R = next byte; register (R - 0x40) & 0xff = expr as u16 (any byte names one of 256 words from 0x445298) |
| 0d | 0x4064f8 | R; segment s = `@[7]`, or 0 when R is `@`, stored to 0x44629a; off = reg R + 2*expr; then for each expression of the 02 list a word at seg_base[[0x44629a]] + off, off += 2 |
| 0e | 0x406557 | R; off = reg R + expr; bytes at seg_base[`@[7]`] + off, off += 1 (no `@` case; the base is read again per byte) |
| 04 11 / 04 12 | 0x406638 / 0x40663b | AL = 1 / 2 and `ret`: leaves the VM running the block, which the enclosing while (04 10) reads |
| 12 | 0x40663e | argument list n, offset: label n = offset |
| 18 | 0x4069be | argument list c: text colour = c |
| 04 20 | 0x4070bc | argument list b, offset: click handler b = offset (no bound check on b) |
| 04 22 | 0x40739b | argument list read and dropped |
| 04 23 | 0x4071a4 | the segment base of `@[7]` is read first; then argument list table, x, y; records of five u16 {id, x0, y0, x1, y1} at base + table, tested x >= x0, x <= x1, y >= y0, y <= y1 in that order, inclusive, until id 0xffff; S = the id found or 0xffff |
| 0f | 0x40659e | expr; nonzero: run the block (0x4061cd), skip the rest of the 02 0f / 02 chain, then go on if AL = 0, else `ret` (break/continue out of the enclosing while); zero: skip the block and test the next 02 0f, or run a final 02 block |
| 11 | 0x4065f6 | wait for a click or Enter pressed and released (0x406acc), then clear the text window (0x406b24) |
| 13 / 14 | 0x406658 / 0x406a9a | run the block at label n / at script offset v with ESI pushed (nested VM), then go on |
| 15 | 0x4069ce | v; format it at 0x445ce6 (0x4069fd): digits from the low nibble of [0x4452f7] (0 as many as needed, 1-4 that many low ones, else 5; a too small count gives a first "digit" above 9), ASCII between 06 marks when the high nibble is set (`inc di`), else Big5 0xa2af + d and a NUL; then run it as a nested script |
| 16 | 0x406aad | v: wait v/4 + 1 frames (0x4075c6, ECX = v); no argument: wait for a click (0x406acc) |
| 04 10 | 0x40660c | expr block: push the statement; while expr, run the block; AL = 1 (continue) re-tests, AL = 2 (break) ends |
| 04 13 | 0x406671 | n: menu block = label n, or the block given inline (0x445e0c); [0x445304] = 1; run label 38; then each round: pump (WM_QUIT ends the VM), step the animations; if [0x445304] was cleared go on, else run label 39 and, for the first input k = 0-5 enabled in 0x4452e1 (bit 1 << (k + 1)) that is down (0x4074d5), run label 32 + k and, unless bit 0 of 0x4452e1, wait for its release (k = 5 steps the animations while it waits) |
| 04 1e | 0x406f7c | name: load it over the script, reset the slots (0x406dff), clear 0x445ce1/ce2/ce4, ESI = the file's entry, `esp = [0x445cdc]` |
| 04 1f | 0x406fcd | name: run it as a subroutine ("Calling a MES file" below) |
| 04 21 | 0x4070d5 | mode, n: 0x445d8a = '0' + n; 0: load FLAGn over 0x444e78 (0xc00 bytes: name, flags, registers), keeping bits 5-6 of @[9], VM init (0x406395), then as 04 1e with the name it holds; 1: save those 0xc00 bytes; other: the load and VM init, keeping the running file's name (copied to 0x445d66 and back) |
| 17 | 0x406b05 | [c]: clear the text window (0x406b24) in colour c << 4, or in the text colour |
| 19 | 0x407204 | n, a, b, result in S: 0 restore the picture stashed in segment 3 and draw it to surface 1; 1 S = 0xff; 2 palette fade / stopwatch (0x401d30, 0x401e10); 3 y swap 512 bytes of row y + 0x40 between surfaces 0 and 1; 4 dissolve wipe (0x401a50); 5 a S = a |
| 1a | 0x407096 | n, slot, a, b: animation slot control 0x403510 (EAX n, EBX slot, ECX a, EDX b) |
| 04 14 | 0x406842 | n: 1 counts the visible items of the menu block into 0x445302; else fills the table at @[22] with {text offset, item number} for [0x445300] visible items after skipping [0x4452fe] (item step 0x4068e1) |
| 04 15 | 0x406916 | n: 0 show / 1 hide the cursor (only if not so), 2 nothing, 3 x y move the mouse, 4 off load the 32x32 AND/XOR bitmap (128 dwords, byte-swapped) from segment @[7] to 0x4be900 |
| 04 16 | 0x406f06 | n [v]: 2 set the palette now (0x401ca0), or all entries from v (0x40754d), saving and restoring the tables (0x40756c, 0x407599); 3 fade and wait; others nothing |
| 04 17 / 04 18 | 0x406b76 / 0x406bcb | x0 y0 x1 y1 (x in units of 8 pixels): fill (0x4017b0) / XOR (0x401870) on surface @[6] with the text colour |
| 04 19 / 04 1a / 04 1b | 0x406c20 / 0x406c85 / 0x406cea | x0 y0 x1 y1 s dx dy d: copy (0x401430) / swap (0x401670) / masked copy (0x4014f0) from surface s to (dx, dy) of d, surfaces as one word s << 8 \| d; 04 1b with no argument redraws the last picture's rectangle (0x44538a) from surface 1 to 2 |
| 04 1c | 0x406d8b | name, off: load the file into segment @[7] at off (0x401050) |
| 04 1d | 0x406e11 | [name]: picture to surface @[6] (0x406e20; no argument: the one at 0x4662be) |
| 04 24 / 04 25 | 0x4073a5 / 0x4073cd | music (0x402cb0 / stop 0x4032c0) / voice clip (0x403360) |
| 04 26 | 0x4073ee | n, args: map sub-op n through 0x403a70 with ESI = the argument values, S = AX after; 4 and 5 first reset the slots (0x406dff); see "Town map" |

### Animation slots (0x403510, 0x403624, 0x403661; reimplemented in src/game/anim.c)

Ten slots of 24 bytes at 0x4312cc: u8 state (0 stopped, 1 stop at the next mark, 0xff
running), u8 delay, u8 loop counters A and B, u32 first instruction, u32 next instruction,
u32 loop starts A and B, u16 x and y (set by sub-op 5, never read). The ANI file (04 1c to
0x4a62be) holds from its byte 1 a u16 offset per script; scripts, rectangles and palettes are offsets
from 0x4a62be. 0x403624 runs once a frame (u32 0x4313bc = the last frame) unless bit 1 of
@[9]: each slot in order through 0x403661, which returns for a stopped slot, counts a delay
down, or runs instructions until one that ends the slot's frame: 00 stop; 01 mark (stops a
slot in state 1); 02 n delay n frames; 03 restart; 04 n / 06 n loop A / B n times from
here; 05 / 07 next A / B (back while the counter is nonzero); 08 off / 09 off copy / masked
copy (colour 8 transparent) the rectangle record {x0, y0, w, h, dx, dy} at off from surface
1 to 0 (0x401430 / 0x4014f0); 0a off the palette at off (16 + 240 u16 into 0x44530c and
0x445eae, 0x40752c), uploaded with 0x401ca0. 08-0a go on in the same frame. 0x403510 (EAX
sub-op, EBX slot): 0 ECX = n load script n stopped; 1 start; 2 stop at the next mark if
running; 3 stop; 4 start and wait (spinning on the frame counter) until it stops; 5 x, y;
6 slots 1-7 stop at their next mark; 7 every slot stopped at the idle script 0x4313c0
(0x406dff: sub-op 7 with slot 0).

### Town map (ext op 04 26; reimplemented in src/game/map*.c)

04 26 n, args (handler 0x4073ee) calls 0x403a70 with ESI = the argument values: it copies
arguments 1-6 (u16) to 0x43555e and calls sub-op n through the table at 0x435576; S = AX
afterwards (so the result of a sub-op is whatever AX it leaves). The scripts use 04 26 3c
(sub-op 12) as the walk loop's frame. Sub-ops: 0 init (0x403a87), 1 patch sprite script 0
(0x403ae9), 2 grid from the layer (0x403b28), 3 sprite tiles over the grid (0x403bd2), 4 draw
changed cells (0x403d45), 5 draw all cells and check events (0x403e30), 6 = 2, 3, 4 (0x4042e7),
7 step the sprites (0x4042f7), 8 area under sprite a (0x404f8c), 9 collision for sprite a
(0x40500e), 10 enter at entry a (0x40502c), 11 history reset (0x405113), 12 one frame (0x4051c2:
step, 2-4, pump, event check 0x405216), 13 sprite a runs script b (0x4053a6), 14 enable /
disable sprite a (0x4053de), 15 unpack a run-length layer (0x405415), 16 copy a rectangle of
cells (0x40548f), 30 plan a walk for sprite a (0x405542), 32 stop the walk (0x40558f).

The map state record M is in script segment 1 (0x4462be) at the word @[5] (0x4452d8): u16 at
+0 the offset of the layer block (u16 w, u16 h, w*h u16 tiles, bit 15 = wall, then the walk grid,
w*h bytes), +2/+4 and +6/+8 the lowest and highest view column / row the view may scroll to, +0xa
the view's x on screen in 16-pixel cells, +0xc its y in pixels, +0xe/+0x10 its size in cells,
+0x12/+0x14/+0x16/+0x18 the scroll margins left/top/right/bottom, +0x1a/+0x1c the map cell at the
view's top left, +0x1e/+0x20 the map size (copied from the layer), +0x28 the sprite pattern
pointer table, +0x2a the frame table, +0x2c the script pointer table, +0x2e the sprite table, +0x30
the area table (x0, y0, x1, y1, code: 10 bytes, ended by x0 = 0xffff), +0x32 the entry table (18
bytes: view x, y; player x, y, frame; the four limits). A sprite is 16 bytes (sprite 0 is the
player, 1 and 2 follow it; a record whose flags are 0xff ends the table): +0 flags (1 active,
2 drawn, 4 the view follows it, 8 no sliding, 0x10 nothing collides with it, 0x20 not solid, 0x80
off), +1/+3 cell x, y, +5 and +0xf destination, +6 pattern set, +7 script, +8 position in it,
+0xa frame (direction * 2: up, down, left, right; bit 0 flips every step), +0xb/+0xc footprint,
+0xd kind, +0xe steps left.

Drawing: the view is a grid of 16 x 16 cells; 0x4313c8 holds 4 words per cell (the layer tile and
up to three sprite tiles, +1), 0x433308 what was drawn. Tiles and sprite tiles are 4-bit pixel
blocks in two locked surfaces (0x43554c, 0x435550; 0x4355fa maps a tile number to the block's
offset in eighths, sheets 640 pixels wide). A cell without sprite tiles is the tile's low 3 bits
over the screen's bit 3; with them the cell is composed in three work planes at 0x435248: a
sprite pixel with bit 3 clear replaces the pixel, one with bit 3 set is transparent; a tile pixel
with bit 3 set stays in front of the sprites; only the low 3 bits reach the screen.

Movement (0x404731 up, 0x404937 down, 0x404a9a left, 0x404c9f right; AL = 0xff moved, 0 blocked):
turn (frame = step bit + direction * 2), refuse at the map edge or if another sprite is in the
way (0x404e25: the cell ahead in the facing; sprites 1 and 2 and ghost, off, not drawn sprites
excluded), walls count only on the feet row (across the width when going up or down, the one
cell ahead sideways); a blocked step slides to a side that is open (0x4047f7 / 0x404a29: cells
left, first, last, right of the footprint; 0x404b55 / 0x404d8a: rows above and below). The
tails scroll the view when the sprite comes within the margin and the view is not at its limit.
Quirk kept: the right mover's test for the map's right edge (0x404cb8, and 0x4048d7 in the slide
to the right) adds the sprite's height, not its width, to x before comparing with the map width
(the fuzzing found it; the walls and the scroll below use the width).
Sprite scripts (0x40431b): high nibble kind (1-4 move, 5 / 7 stand, 6 the player, 8-11
diagonals as two steps, 13 walk to the destination, 0xc history record, 0xe random kind 1-4
with the generator at 0x435e60, 0xf set frame / show / hide), low nibble the count.

Walking (0x405966, 0x405691): the walk grid is built from the layer (a cell is blocked, 0xff, if the
tile below it or the one right of that has the wall bit; else 0xfe); the sprite's cell is cleared
to 0 and a recursive search from the goal (0x405b07, depth 0xfd down, neighbour order by a 3-bit
mask of the direction toward the sprite) marks the cells on the first route it finds with their
depth and the ones it tried 0xff. A step goes to the neighbour with a higher mark below 0xfe
(diagonals first, as two orthogonal steps). If the goal is a wall and 0x435e61 bit 0 is set,
0x405fb6 marks free cells around it outward. Quirk kept: the second pass reads the goal's row from
DX, which the first `mul` clobbered (row 0 in practice). 0x435573 = 0xff while a route is walked.

The pumping chain (stage 6; src/game/map_pump.c). Sub-ops 5, 7 and 12 are the ones that wait for the
window. The pump 0x40609c is `pushad; call 0x401bf0; (0: WM_QUIT: mov esp, [0x445cdc]; pop ebp;
ret at 0x404729); popad; BL = (input & 0x10 ? 1 : 0) | (input & 0x20 ? 2 : 0), BH = input & 0x20 ?
2 : 0; CX, DX = the mouse (0x4452e2, 0x4452e4); ret`: bit 0 of BL is the left button or Enter,
bit 1 the right button or Escape; the other registers are as before. Sprite kind 6, the player
(0x404450), saves frame, x, y (three pushes, popped at the end for the history record), then: with
the game's cursor off (0x445ce0 = 0) the keyboard: pump, then the arrows at 0x445d8f bits 4, 8, 1, 2
(left, right, up, down) each call their mover and CH collects the movers' AL; with the cursor on
a walk along the walk grid (0x435573 = 0xff: 0x405691, then the frame bit flips) or the mouse:
pump, the right button starts a walk to the cell under the mouse if the mouse is inside the view
(0x405597: view x * 16 .. (view x + width) * 16, view y .. + height * 16; goal = mouse / 16 - screen
position + view position; 0x435e61 = 0 so a wall goal does not get approach cells), pump again, the
left button moves the player one step toward the mouse (compare the mouse with the player's place
on the screen, the margins 0x30 and 0x20 + 0x60 wide: vertical first - up if the mouse is above,
down if below the box - then sideways; 0x435572 marks that the vertical test lies above the
margin and keeps the 0x30 off the box). A step that moved (CH != 0) sets 0x435571 = 0xff, stops the walk
(0x435573 = 0), flips the frame bit and shifts the position history (0x435dca, 5 bytes a record,
10 records) by one with the old frame, x, y first. The event check 0x405216 runs at the end of
sub-ops 12 and 5: collision test of sprite 0 (0x40500e; a contact is the sprite number, else
0xffff); on a contact it pumps and, if the mouse is on the player and the left button is down
and the player did not move this frame (0x435571), the contact is the event (EAX); the click
is remembered at 0x43556c and the area under sprite 0 (0x404f8c) at 0x43556e (a new area gives
its code + 0x32); the player's facing (frame / 2) at 0x43556a; an event is held until the left
button is released (0x405399 pumps while BL bit 0). Sub-op 12 first pumps once after drawing:
the left button inside the box x 0xf8-0x187, y 0x120-0x13f holds the frame (EAX = 0xfffe) until it
is released, any other click goes to the event check. Native call frames of the chain: the
handler pushes ESI and `call 0x403a70` (return 0x40740f), 0x403a70 calls the table entry (return
0x403a86), the sprite loop 0x4042f7 pushes ESI and calls 0x40431b (return 0x404314), which calls
0x404410 (return 0x40440f) and jumps into the kind's code; sub-op 12 calls the loop with return
0x4051ce, sub-op 5 calls the event check with return 0x403ed3 (after `push ebp`), the right click
is a call with return 0x404495.

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
memory. The pump is not the only way there: a 04 11/04 12 in an if block at the called
file's top level returns through 0f's `ret` (0x4065bf) with whatever ECX the handlers
before it left, so every handler must leave ECX's high half zero, as the native ones do
(the differential harness checks it, `diff_live_bits`).

## Running the game code: interpreter, recompile, lockstep (historical: removed in R2)

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

### nanpa2.dat: the exe's built-in data without its code (src/asset.c)

Little-endian. Header 48 bytes: `"N2AS"`; u32 version 1; u32 PE TimeDateStamp 0x39697c6a; u32
SizeOfImage 0xc1000; u32 size of the source exe; u32 range count; u64 FNV-1a of the source exe;
u64 FNV-1a of everything after the header; u32 payload bytes; u32 0. Then the range table, 12
bytes each (u32 virtual address, u32 size, u32 kind: 0 bytes stored in the payload, 1 zeros, not
stored), then the payload (the kind 0 ranges' bytes in table order). The loader allocates the
zeroed image (0x400000-0x4000000) and copies each range to its address; everything else
(the code, the bss, the heap) is zero. It refuses any range that overlaps 0x401000-0x4077ff,
reaches outside the 0xc1000-byte image or does not add up to the file size, and any file whose
hash does not match. The writer keeps 0x400000-0x401000 (PE header) and 0x41b000-0x4ab000
(.rdata and the initialized .data), split into data and zero runs of 32 bytes or more: 66 ranges,
95,213 data bytes (546 header, 82,497 .rdata, 12,170 .data), 96,053 bytes in all. Not kept: .text
0x401000-0x41b000 (game code, MP3 library, C runtime), the .data bss tail 0x4ab000-0x4c0000 and
.rsrc. Established by image traces (docs/reimpl.md "The asset").

## Palette and fades (0x401ca0, 0x401d30, 0x40752c-0x407599)

- Colour tables: 0x44530c u16[16] (interface colours, 0x0GRB, one nibble per channel) and
  0x445eae u16[240] (picture colours, 5:5:5, red bits 10-14). 0x401ca0 builds
  PALETTEENTRY[256] at 0x4ac07c from them: base entry red = w & 0xf0, green = (w >> 4) & 0xf0,
  blue = (w & 0xf) << 4; picture entry red = (w >> 7) & 0xf8, green = (w >> 2) & 0xf8, blue =
  (w & 0x1f) << 3; then IDirectDrawPalette::SetEntries(0, 0, 256, entries) on the object at
  0x4ac4f4.
- 0x40756c / 0x407599 copy both tables to / from 0x44608e (unless @[9] bit 4, 0x4452e0 & 0x10),
  0x40754d fills both with AX.
- Fade (0x401d30): i32[256][3] at 0x4ab060 = (target channel from the tables) - (displayed
  entry), the displayed entries copied to 0x4aac58 and 0x4ac504 = 1. The timer callback
  (HLE, native 0x4028c0) steps 16 times every 0x44628e ticks and clears 0x4ac504. Op 04 16 3
  = save tables, [fill], steps = 1, fade, wait for 0x4ac504, restore tables.

## Surfaces and cursor (0x401190-0x401c80, 0x401430-0x401900)

- Surface objects at 0x44629e (u32[], 0 = screen), pitches at 0x4462ae. Lock descriptor
  0x4ac480 (lpSurface 0x4ac4a4). 0x4ac068 set: the screen is drawn 40 lines down (rectangles
  and positions on surface 0, and the Lock pointer, +40 lines).
- Game cursor: shown flag 0x4ac510, position x 0x4ac064 / y 0x4ac060, frame 0x4ac514 (blinks),
  saved position 0x4aac54 / 0x4aac50, saved pixels 0x4abc60 (32 rows of 32 bytes), shape
  0x4be900 + frame * 256: u32 row words, plane 1 (white) then plane 2 at +0x80 (black where
  the bit is clear), bit 31 = leftmost. Every routine that locks or Blt's the screen takes it
  off (0x4011f0) and puts it back (0x4012e0).
- Copy 0x401430 / masked 0x4014f0 / swap 0x401670 (cdecl dx, dy, sx, sy, w, h, surfaces =
  src << 8 | dst): BltFast; masked on the screen uses the colour key (flags 0x11), otherwise
  by hand with colour 8 transparent; swap goes through the scratch surface 0x4ab058. Fill
  0x4017b0 (x, y, w, h, colour << 4, surface): Blt COLORFILL. XOR 0x401870: dwords, width
  rounded up to 4.
- Picture 0x406e20: header u16 x / 8, y, w / 8, h at 0x4662be (copied to 0x44538a-0x445390),
  palette block at +8 (0x40752c unless @[9] bit 0), pixels at +0x208.

## File access, window procedure and start-up (0x401000-0x401160, 0x4021c0, 0x402090-0x402a20)

- File 0x401000(name): fopen(name, "rb") (mode string 0x43103c); if it fails,
  sprintf(buf, "%s%s", CD prefix 0x431030, name) and fopen that. 0x401050(name, buf): path =
  name if its first 4 characters are "DATA" (case-insensitive, 0x431058), else "DATA\%s"
  (0x431050); open as above; nothing happens if it cannot be opened. A name whose last
  extension is .GP4 (0x431048) or .ANI (0x431040) is read whole into a malloc block and
  decompressed into buf (0x4075f0), anything else is read into buf. 0x401160(name, buf, size):
  fopen "wb" (0x431060), fwrite(buf, 1, size), fclose; the name is used as given. Register
  effects the callers see: ECX = buf after a plain read, = the path buffer (native local_28)
  after the "DATA\%s" sprintf or a CD-prefix retry, 0 after the copy of a name already under
  DATA; EDX = the path buffer.
- Window procedure 0x4021c0 (stdcall hwnd, msg, wparam, lparam, `ret 0x10`): 0x200 mouse
  move: x = low word -> 0x4ac064 and u16 0x4452e2; y = high word -> 0x4ac060; u16 0x4452e4 =
  y, minus 40 when 0x4ac068 is set, clamped to 0..399; if the game cursor is shown (0x4ac510)
  the repaint 0x402570 (IDirectDraw::WaitForVerticalBlank(1, 0), cursor restore 0x4011f0,
  cursor draw 0x4012e0). 0x201/0x204 left/right button down: voice stop 0x403440, set input
  bit 0x10/0x20 in 0x445d8f; 0x202/0x205 up: clear it (no voice stop). 0x100 key down (VK -
  0x0d must be <= 0x63): up 0x26 -> 1, down 0x28 -> 2, left 0x25 -> 4, right 0x27 -> 8,
  Enter/Space 0x0d/0x20 -> 0x10, Esc/Insert 0x1b/0x2d -> 0x20, Ctrl 0x11 -> 0x40, End 0x23 ->
  0x80 (OR into 0x445d8f); F1 (0x70) DestroyWindow. 0x101 key up (VK - 0x0d <= 0x20): clears
  the same bits, with the voice stop for Enter/Space/Esc/Insert. 2 WM_DESTROY: PostQuitMessage(0).
  0x1c WM_ACTIVATEAPP: wparam 0: 0x4ac518 = 1, 0x4ac4ec = 0, voice stop, music pause 0x403220;
  else (if 0x4ac518) music resume 0x403250, SetFocus(hwnd), 0x4ac4ec = 1. Everything else:
  DefWindowProcA. EAX = 0 except the DefWindowProc result.
- Start-up 0x402090 (WinMain): 0x4ac06c = hInstance; WNDCLASS {style 0, proc 0x4021c0, 0, 0,
  hInstance, LoadIcon "IDI_ICON1", LoadCursor(IDC_ARROW), WHITE_BRUSH(4), no menu, "Nanpa2"};
  RegisterClassA (0 -> return -1); CreateWindowExA(0, "Nanpa2", "Nanpa2", WS_POPUP, 0, 0, screen
  size) -> 0x4ab05c; CD search 0x402590 (0 -> MessageBox, return 0); logo 0x401e20; display
  init 0x402620 (0 -> return 0); ShowWindow(5), UpdateWindow; the VM 0x4060d4; shutdown
  0x402a20; return 1. CD search: for drive letters A to Z, 0x431030[0] = the letter,
  GetDriveTypeA == 5 (CD-ROM) and fopen of "%sSPEECH\A\A10930.WAV" succeeds; else prefix "" and
  the same file in the current folder.
- Display init 0x402620: ShowCursor(1); DirectSound init 0x402ad0(hwnd); DirectDrawCreate;
  SetCooperativeLevel(hwnd, 0x11); SetDisplayMode(640, 480, 8); 0x4ac068 = 1; primary surface
  (desc size 0x6c at 0x4ac480, flags 1, caps 0x200) -> 0x44629e; Restore; Lock/Unlock to read the
  pitch -> 0x4462ae; colour key {8, 8} on it; 256 black palette entries at 0x4ac07c; CreatePalette
  (0x44) -> 0x4ac4f4, SetPalette; off-screen surfaces (flags 7, caps 0x840) 640 x 400 x 3 at
  0x4462a2.. (pitch 640 each, colour key {8, 8}), 640 x 480 scratch surface at 0x4ab058; fonts
  15, 24, 32 px (weights 700, 700, 400, charset 0x88) at 0x4ac070..; timeSetEvent(16, 16,
  0x4028c0, 0, 1) -> 0x4ac500; ShowCursor(0). Shutdown 0x402a20 reverses it (timer, fonts,
  palette, scratch, surfaces, DirectDraw, sound exit 0x402c70, ShowCursor(1)).

## Dissolve wipe (0x401a50, op 19 sub-op 4; src/game/wipe.c)

`call 0x401a50` with two surface indices (the handler pushes 2 then 0: copy surface 2 onto
surface 0; the arguments are read at [esp+0x4c] and [esp+0x54] after the pushes). Both surfaces
are locked (0x401190) for the whole wipe and unlocked (0x4012b0) at its end. 128 passes; a pass
copies, in 25 rows of the 400-line picture (every 16th line, starting at line `ebx * pitch`,
640 pixels wide), the pixels whose bit is set in a 16-byte dither mask: the mask byte `ebx`
gets one more bit per pass (a walking bit, 0x80 down to 1 and round again) and `ebx` advances by
5 mod 16 (plus 1 each time the walking bit wraps), so every line has every bit after 128
passes. After a pass the wipe waits until two frames have passed since the pass started (the
frame counter 0x446292), pumping messages (0x401bf0) each round; an input bit in 0x30 (OK or
cancel) sets a flag that skips all later waits, so the rest of the passes run at once. The
pump's result is never read (a WM_QUIT is seen by the VM's next pump). 166,485 instructions for
the set-up and 128 passes with no waits; with waits the pump rounds dominate.

## The data area as one object (R1; src/game/state.h)

0x431000-0x4c0000 (.data and bss; 0x8f000 bytes) is `struct game_state G`, in native layout, with
these members (all offsets static-asserted): `head` 0x431000 (strings and formats, the CD prefix
at 0x431030), animation slots 0x4312cc-0x4313c6, the map's cell grid 0x4313c8 (0x1f40 bytes, 4
words per cell) and shadow 0x433308 (0x1f40), work planes 0x435248 (3 x 256), the map variables
0x435548-0x435e78 (locked pixel pointers 0x435548/4c/50, layer tiles 0x435556, the 04 26
arguments 0x43555e, event words 0x43556a-6e, mover flags 0x435570-73, the walk grid offset
0x435574, the sub-op handler table 0x435576 (33 native addresses), the tile offset table
0x4355fa (1000 u16, read-only), the position history 0x435dca (10 x 5 bytes), the random state
0x435e60, the approach flag 0x435e61, the planner words 0x435e62-72), script segment 0
0x435e78-0x444e78, the save image, 0x445a78-0x445cdc (unused), the VM block, segment 1
0x4462be (64 KB), segments 2 and 3 0x4562be (320 KB), the ANI file 0x4a62be-0x4aac50, the screen
globals, the sound words 0x4be540-0x4be900 (only the native DirectSound code used them), the
cursor bitmap 0x4be900 (128 dwords, byte-swapped by op 04 15 4) and the rest of the C runtime and
MP3 library data. The sections of the exe the game never writes (.rdata, the strings in `head`,
the tile offset table) are asset data.

## Findings of R4 (the platform API replaced the C runtime and Win32 thunks)

Facts about the exe that showed up when the C runtime calls of `file.c` and `startup.c` were written as
plain C:

- **The file loader's name test is `FLAG`, not `DATA`.** 0x401050 compares the first four characters of
  the name with the string at 0x431058, which is "FLAG": a name that starts with it (the save slots) is
  opened as given; every other name is `sprintf`ed with the format at 0x431050, "DATA\%s". The format at
  0x431034 is "%s%s" (the CD prefix at 0x431030, initially "A:\", then the name); the modes are "rb" (0x43103c)
  and "wb" (0x431060); the extensions tested for decompression are ".ANI" (0x431040) and ".GP4"
  (0x431048). If the open fails, the second attempt is the prefix and the name; both attempts count as
  opens in the replay's `li.files`.
- **The decompressor's input length.** 0x401050 reads a packed file into a `malloc` block of the file's
  size and passes that block to 0x4075f0, whose bit reader takes its length from the block: the heap size
  rounded to 16 including an 8-byte header, minus the header (`((size + 8 + 15) & ~15) - 8`), the file's bytes
  followed by zeros. The pacing model's decode cost uses that length (`PACE_DECODE_US(in, out)`).
- **The CD search** tries drive letters A-Z in order, writing the letter into the prefix at 0x431030, and
  only a CD-ROM drive (GetDriveTypeA = 5) opens "%sSPEECH\A\A10930.WAV" (0x431188 is the format); the port
  answers CD-ROM for drive Q. If none opens, the prefix is cleared and "SPEECH\A\A10930.WAV" (0x431174)
  is tried in the current folder.
- **The fade step factor** at 0x41b188 is the float 0.0625 (1/16): the timer callback computes
  `delta * (step * k) + start` in double.
- **Script register S after a surface unlock.** 0x4012b0 returns the screen's DirectDraw object (a heap
  address) when the surface unlocked is not the screen, 0 when the cursor is off, else the cursor's x; ops
  19 0, 19 3 and 19 4 and the map's redraws store the low word in S. The value is in the saved image and a
  script could read it, so the port's primary surface handle is a constant with the value the emulated
  heap gave it (`SURF_PRIMARY_HANDLE`).
- **The FLAG file** is the 0xc00 bytes from 0x444e78: name 0x0, flags 0x20, registers 0x420, the word array
  0x456, the base palette 0x494, the four segment bases 0x4c4 (the only addresses in it; rewritten by the VM
  set-up after every load), the last picture rectangle 0x512, the work buffer 0x51e. The words 0x445a78-0x445cdc
  after it and the native top ESP at 0x445cdc are not saved.
