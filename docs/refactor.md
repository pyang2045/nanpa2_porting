# Refactor: from the emulated machine to a free-standing C engine

The game logic is C (docs/reimpl.md) but still runs inside the model of the 32-bit PC it was
written for: its state is bytes of emulated memory at native addresses (`rd32(0x446292)`), its
units pass values in emulated registers and push native stack frames, its pace comes from
instruction charges (`X86_COUNT`) at 25 MIPS, and Win32, DirectDraw and the C runtime are reached
through thunks by native address. The refactor removes that model in stages. Because the
reimplementation is then no longer byte for byte the original, the differential harness
(`NANPA2_DIFF`) stops applying and every stage is checked by **replay** instead.

| Stage | What | State |
|---|---|---|
| R0 | deterministic mode, recordings with state checkpoints, replay and compare, a golden set | done |
| R1 | game state in C structs instead of emulated memory | started: four groups moved, the rest listed below |
| R2 | ordinary C calls instead of register and stack conventions (`cpu.r[]`, `push32`, `go(ra)`, jump units) | not started |
| R3 | real-time pacing that keeps the original speed (no per-unit charges) | not started |
| R4 | direct calls into the platform layer instead of HLE thunks | not started |

## R0: deterministic mode, recording and replay

Long headless runs used to be host-timing lotteries: the 16 ms timer (frame counter, palette
fades, cursor blink) fired on `SDL_GetTicks()`, so the number of instructions between two frames
varied, and the exploration bot and `NANPA2_INPUT` posted their messages at host milliseconds.
`NANPA2_DETERMINISTIC=1` (implied by `--record` and `--replay`) removes every host clock from the
game's behaviour:

- **The game clock is the instruction count**: `replay_ms()` = `x86_icount / 25000`, a fixed
  25 MIPS whatever `--cpu-mips` or `NANPA2_TURBO` say (turbo is off in this mode). The timer
  callback runs from `win32_run_timer` against that clock. `x86_throttle` only sleeps, so a
  throttled and an unthrottled run are identical (checked: 1000 frames at 25 MIPS and at
  `NANPA2_CPU_MIPS=0` give the same file).
- **Input is taken at pumps, not milliseconds**: scripts (`NANPA2_INPUT`, clock forced to the
  game's frame counter) and the exploration bot run at every `win32_pump`; the host event queue
  every 256th. Every message that comes from outside the game goes through `win32_post_ext`.
- Audio runs on its own thread and writes nothing the game reads (no sound call returns state
  into the VM), the status watchdog only reads, and presentation is not game state: nothing
  to do for them. The bot's own expression evaluation used to advance the instruction count
  (a replay has no bot, 77 instructions per menu of drift): it is now excluded
  (62f125d).

**Recording** (`--record FILE` or `NANPA2_RECORD`): a text file. Header: `nanpa2-replay 1`,
`virt_ips`, `cp_ticks`, and `saves N/HASH`, the FLAG files of the save folder at the start (a replay
refuses a folder that is not as recorded: a save-and-load run replayed from a mutated folder would
diverge silently). Then in order:

    ev TICK PUMP MSG WPARAM LPARAM      a message from outside (hex), at game tick TICK (timer
                                        callbacks run) and the PUMP-th pump of that tick
    cp TICK PUMP key=value ...          a checkpoint, every N ticks (NANPA2_CP_TICKS, 250), taken
                                        in the first pump after the tick crosses a multiple of N
    fin TICK PUMP key=value ...         the same at exit

Messages are in Windows terms (WM_MOUSEMOVE 0x200, WM_LBUTTONDOWN 0x201, WM_KEYDOWN 0x100, WM_QUIT
0x12), so a recording is independent of the engine's memory. It holds no pixels, no memory and no
game text: only MES file names are hashed, never stored, and the ctest builds its own at test time.

**Checkpoint keys.**

| Key | Hash of | Survives the refactor |
|---|---|---|
| `li.frame` | the game's frame counter | yes |
| `li.screen`, `li.pal` | the primary DirectDraw surface (640x480 indices) and the palette | yes |
| `li.save` | the 0xc00-byte save image (`state_save_image`: script name, flags, registers, the word array with the text window), what a FLAG file holds | yes (the serializer defines it) |
| `li.mes`, `li.files` | the ordered list of MES files (and of all files) the game opened, with counts | yes |
| `li.vm` | script opcode fetches so far / the last offset | yes |
| `li.flags` | the FLAG files in the save folder, read from disk | yes |
| `raw.icount`, `raw.cpu` | instruction count, CPU registers | not once R2/R3 change the charges |
| `raw.data`, `raw.pg` | emulated .data/.bss 0x431000-0x4c0000 (whole, and 16 bits per 4 KB page), read through the aliases | while the state is at its native layout (all of R1 so far) |
| `raw.heap`, `raw.stack` | heap up to `x86_heap_top()`, the stack from ESP | same |

Only `src/game/state_view.c` knows where the `li.*` state lives, so a chunk of R1 changes that
file and not the checkpoints. `NANPA2_REPLAY_CHECK=li` compares only the `li.*` keys.

**Replay** (`--replay FILE` or `NANPA2_REPLAY`): all outside sources are off (host input,
scripts, the bot; only a window close is honoured), the recorded messages are posted at their
tick and pump, the throttle is off unless `--cpu-mips` is given, and every checkpoint is compared
with the recorded one. The first difference stops the run with exit code 3:

    replay: DIVERGED at checkpoint 2 (tick 750 pump 0): li.screen golden=... live=...
    replay: DIVERGED at checkpoint 1 (tick 500 pump 0): raw.icount ... raw.pg: 4ac000

(`raw.pg:` lists the 4 KB pages of .data/.bss that differ.) `tools/replay_diff.py A.rec B.rec
[--li]` does the same for two recordings. A recording of a run and its replay with `--record`
are identical files.

**Limits.** A recording is tied to `VIRT_IPS = 25e6` and to the instruction charges of the
units: the checkpoint falls at a pump, and where the game is between two pumps at that moment
depends on the charges. R1 moves state and keeps the charges, so `raw.*` stays valid through
it; R2 and R3 change the charges, so after them the golden set must be re-recorded from the last
R1 build (and `li.*` compared only for the first checkpoints).

**ctest `replay_det`** (about 35 s, headless): the exploration bot plays a new game for 2500
frames and is recorded twice (the recordings and the FLAG files must be identical), the first
is replayed (every checkpoint matches), one with a changed `li.screen` hash and one with a click
left out must diverge (exit 3).

### The golden set

Made on the pre-refactor code (b98343c plus the bot fix 62f125d and the R0 commit), headless,
`NANPA2_CPU_MIPS=0`, from copies of a save folder; kept outside the repository (they hold derived
game state). Recording: `NANPA2_EXPLORE=SEED NANPA2_EXPLORE_FRAMES=N NANPA2_CP_TICKS=C
nanpa2 --game game --record NAME.rec`; check: `nanpa2 --game game --replay NAME.rec` with the same
starting save folder.

| Name | Start | Covers | Checkpoints | Messages |
|---|---|---|---|---|
| `newgame` | empty save folder, bot seed 31, 270000 frames | title, name entry, the whole Prologue (S1-S63, 68 MES files), OPEN1 at frame 252442, saves (FLAG0 and FLAG6 written) | 271 (every 1000 ticks) | 25842 |
| `town` | saves32 (FLAG0, FLAG6), seed 43, 300000 frames | the Prologue, then the town (TOWN.MES at frame 263535: the map, the player walking by mouse), 39.MES, PARA_SUB, IZUMI; saves | 301 | 28009 |
| `saveload` | saves32, seed 41, 8000 frames | a load: the title's Main item reads FLAG and goes straight to OPEN1 (frame 594); saves are covered by `newgame` and `town` (FLAG0, FLAG6 written, `li.flags` changes) | 33 (every 250) | 674 |
| `menu` | saves32, seed 44, 40000 frames | the title and the menus of the Prologue scenes (S46-S53), 236 menu and click decisions | 161 (every 250 ticks) | 3698 |

## R1: game state in C structs

**The mechanism.** `src/game/state.h` defines the structs and the global `G`. A group that has
moved is an **alias** (`x86_alias(addr, &G.group, size)`, `state_init()` after the asset or exe
is loaded): the range of emulated addresses it occupied is backed by the object, and `x86_ptr`
(so every `rd/wr`, bulk copy, HLE call and script-VM segment access) hands out the object's bytes
for it. The lookup is one comparison for addresses outside 0x431000-0x4c0000 and a table lookup
inside; an access must lie wholly in one alias or wholly outside all (a straddling access
traps). So code that has not been converted keeps working unchanged, converted code names the
field (`SV(text_x)`, `RT(frame)`, `SC_SET(fade_step, n)`, `G.save.reg[r]`), and the structs may later
take any layout once nothing names the address. Layout is pinned by `_Static_assert(offsetof ==
native address - base)` for every field, so the raw hashes stay valid and a wrong field is a
compile error.

**Moved** (each verified by all three golden replays, `li.*` and `raw.*`):

| Struct | Native range | Holds |
|---|---|---|
| `save_image` (`G.save`) | 0x444e78-0x445a78, 0xc00 | script name, 4-bit flags, registers `@`-`Z`, the word array `@[i]` with the named words: map selector, surface select, text delay, VM flags, menu inputs, mouse, the text window and cursor, text colour, size, line height, the menu fields, the base palette, segment bases, the last picture rectangle, the work buffer. **This is the FLAG file.** |
| `vm_rt` (`G.rt`) | 0x445cdc-0x4462be | VM top ESP, cursor and click-busy flags, click handlers, argument lists, name buffers, input bits, menu list, label tables and their save area, the picture and saved palettes, fade length, the frame counter, stopwatch, store segment, the surface and pitch tables |
| `screen_state` (`G.sc`) | 0x4aac50-0x4ac51c | cursor background and position, palette start/delta/entries of the fade, scratch surface, window handle, fonts, the DirectDraw lock descriptor, the active/paused flags, the fade and cursor-blink state the timer callback steps |
| `anim_state` (`G.an`) | 0x4312cc-0x4313c6 | the ten animation slots, last frame, idle word, the script random seed |

**Save files.** The 0xc00-byte `save_image` *is* the file (script op 04 21 writes and reads
exactly those bytes: `game_file_save`/`game_file_load` pass its emulated address to the C
runtime, which now lands in the struct). The packed layout and the offsets are static-asserted, so
existing saves load by construction; `state_save_image()` (state_view.c) is the explicit
serializer the checkpoint uses and R2's file functions should call. `NANPA2_SAVETEST` and
`replay_det` (FLAG files compared between runs) cover the round trip.

**Named but still addressed.** Accesses whose offset is computed (`rd8(VM_ARG_TYPES + n)`
deliberately runs past 16 arguments into the values, as natively; animation slots indexed by a
script number that is not range checked) stay on the address path through the alias, with the
struct holding the bytes. Arrays (`G.rt.surface`, `G.sc.pal_*`, ...) are converted where the index
is bounded.

**Still in emulated memory** (R1 to do, in the order that reads best):

1. The script image 0x435e78 (segment 0, 60 KB; the VM and `@` address it by offset, and
   segment 0 overlaps `G.save`), segment 1 (0x4462be: the map record, tile layers, walk grid and
   sprite table, `map*.c` ~460 accesses) and segments 2 and 3 (the 320 KB image buffer
   0x4562be, the ANI file at 0x4a62be, the GP4 work areas): to become buffers owned by a
   `script`/`map`/`picture` struct, with typed views of the map record and sprites.
2. The palette tables `G.save.n.palette` (converted) and `palette_picture`/`palette_saved`
   (named, still read by address in screen.c and anim.c), the DirectDraw object tables
   (`G.rt.surface`, `G.rt.pitch`) which hold emulated COM object addresses until R4.
3. The .data between 0x431000 and 0x4312cc (file name formats and strings, read-only: stay as
   tables behind accessors), the CD prefix, the sound state at 0x4be540-0x4be580 (sound.c
   reads it by address) and the file/surface structures the HLE layer allocates on the emulated
   heap (DirectDraw surfaces, COM objects, FILE handles: gone with R4).
4. The CPU registers and the emulated stack (R2), `x86_icount` (R3).

**Boundary.** The HLE layer and the C runtime still address emulated memory; the aliases are
how they see moved state. Nothing outside `src/game`, `src/hle` and `src/x86` names `G`.

## Tooling after R1

| Tool | State |
|---|---|
| ctest `no_game_bytes`, `asset_only`, `wavcheck`, `selftest`, `start_trace`, `replay_det` | kept, pass on the interp build |
| golden replays | the check for every chunk |
| differential harness (`NANPA2_DIFF`, `diff_replay`, `NANPA2_DIFF_DIR`) | **frozen** at b98343c: it snapshots and compares `x86_mem` directly, which no longer holds aliased state, and the C is no longer meant to equal the lifted original. Not built into any test. |
| `NANPA2_FUZZ`, `map_fuzz.c`, `NANPA2_LOCKSTEP`, the recomp backend | **frozen**: they share the harness's direct memory access and need the exe; the sources stay for reference and compile, nothing exercises them. Revive by reading memory through `x86_read_virtual`. |
| `tools/reimpl_status.py` | unchanged (counts what the exe's code still contributes: nothing) |
