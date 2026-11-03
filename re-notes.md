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
