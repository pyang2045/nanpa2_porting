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
