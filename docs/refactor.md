# Refactor: from the emulated machine to a free-standing C engine

(This is the plan and the log of the refactor as it went; the "R4" section at the end describes the
engine as it is now, and its last subsection is the architecture summary.)

The game logic is C (docs/reimpl.md) but ran inside the model of the 32-bit PC it was
written for: its state is bytes of emulated memory at native addresses (`rd32(0x446292)`), its
units pass values in emulated registers and push native stack frames, its pace comes from
instruction charges (`X86_COUNT`) at 25 MIPS, and Win32, DirectDraw and the C runtime are reached
through thunks by native address. The refactor removes that model in stages. Because the
reimplementation is then no longer byte for byte the original, the differential harness
(`NANPA2_DIFF`) stops applying and every stage is checked by **replay** instead.

| Stage | What | State |
|---|---|---|
| R0 | deterministic mode, recordings with state checkpoints, replay and compare, a golden set | done |
| R1 | game state in C structs instead of emulated memory | done: all of the data area is one C object; what stays addressed and why is listed below |
| R2 | ordinary C calls instead of register and stack conventions (`cpu.r[]`, `push32`, `go(ra)`, jump units) | done: see "R2" below |
| R3 | real-time pacing that keeps the original speed (no per-unit charges) | done: see "R3" below |
| R4 | direct calls into the platform layer instead of HLE thunks; no emulated address space | done: see "R4" below |

## R0: deterministic mode, recording and replay

Long headless runs used to be host-timing lotteries: the 16 ms timer (frame counter, palette
fades, cursor blink) fired on `SDL_GetTicks()`, so the number of instructions between two frames
varied, and the exploration bot and `NANPA2_INPUT` posted their messages at host milliseconds.
`NANPA2_DETERMINISTIC=1` (implied by `--record` and `--replay`) removes every host clock from the
game's behaviour:

- **The game clock is the virtual time** (R0 to R2: the instruction count at a fixed 25 MIPS; since
  R3 the microseconds of the pacing model, see "R3"): `replay_ms()` = `clock_us() / 1000`, whatever
  `--speed` says. The timer
  callback runs from `win32_run_timer` against that clock. the throttle only sleeps, so a
  throttled and an unthrottled run are identical (checked: 1000 frames at 25 MIPS and at
  `NANPA2_CPU_MIPS=0`, the R0 names of speed 1 and 0 give the same file).
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
tick and pump, the throttle is off unless `--speed` is given, and every checkpoint is compared
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

**One object.** `struct game_state` (`G`) is the whole of the data and bss of `nanpa2.exe`
that the game writes, 0x431000-0x4c0000 (585 KB), in its native layout, backed by a single alias
(`x86_alias(DATA_BASE, &G, sizeof G)`; `x86_ptr` has a one-comparison fast path for it). Because
the layout is native, every address the original computed lands where it did, including the
unchecked script-supplied indices (see "Out-of-range indices"). Members, in address order, each
offset static-asserted:

| Member | Native range | Holds |
|---|---|---|
| `head` | 0x431000-0x4312cc | .data head: file name formats and strings (read-only, asset-backed) and the CD prefix at 0x431030 (written once at start-up) |
| `an` (`anim_state`) | 0x4312cc-0x4313c6 | the ten animation slots, last frame, idle word, the script random seed |
| `map` (`map_data`) | 0x4313c6-0x435e78 | the cell grid and its shadow, the three drawing work planes, and `map.v` (`map_vars`): locked surface pointers, the 04 26 arguments, event state, mover flags, the sub-op table, the tile offset table (read-only), the position history ring, the path planner's words |
| `script` | 0x435e78-0x444e78 | MES image, segment 0 (the VM addresses it by offset; offsets above 0xf000 run on into `save` and `rt`, as natively) |
| `save` (`save_image`) | 0x444e78-0x445a78 | **the FLAG file**: script name, 4-bit flags, registers, the word array with the named words (map selector, text window, colours, menu fields, the base palette, segment bases, ...) |
| `rt` (`vm_rt`) | 0x445cdc-0x4462be | VM top ESP, cursor and click flags, click handlers, argument lists, name buffers, input bits, menu list, label tables, `palette_picture`, `palette_saved`, fade length, frame counter, stopwatch, store segment, surface and pitch tables |
| `seg1` | 0x4462be-0x4562be | script segment 1: the map record, tile layers, walk grid, sprite table |
| `img` | 0x4562be-0x4a62be | segments 2 and 3, the 320 KB picture and GP4 work buffers |
| `ani` | 0x4a62be-0x4aac50 | the ANI file |
| `sc` (`screen_state`) | 0x4aac50-0x4ac51c | cursor background and position, the fade (`pal_start`, `pal_delta`, `pal_entries`), scratch surface, window, fonts, the DirectDraw lock descriptor, active/paused, the fade and cursor-blink state the timer callback steps |
| `snd` | 0x4be540-0x4be900 | the native sound words and buffers: nothing in C reads them (src/hle/sound.c replaced that code) |
| `cursor_bits` | 0x4be900-0x4beb00 | the 32x32 AND/XOR cursor bitmap (script op 04 15 4) |
| `pad_*` | the rest | gaps and the C runtime / MP3 library data, never touched |

Named in C (no address): `G.save.*` (`SV`), `G.rt.*` (`RT`), `G.sc.*` (`SC`), `G.an.*` (`AN`),
`G.map.v.*` (`MV`), the three palette tables and the fade (screen.c, anim.c), the cursor bitmap
and its saved background, the position history (`hist_*` in map.h). The invariant is checked:
`NANPA2_DATA_CHECK=1` snapshots the data area of the emulated memory at the start and lists, at
the end, every range outside an alias that changed; `=2` makes it exit 4. ctest `replay_det`
runs its replay with it, so a future access that bypasses `G` fails the test. All four golden
replays also run with it (0 ranges).

**Save files.** The 0xc00-byte `save_image` *is* the file (script op 04 21 writes and reads
exactly those bytes). The packed layout and the offsets are static-asserted, so existing saves
load by construction. The file functions are still the generic ones of file.c (the same loader
reads MES and GP4 files into guest buffers and goes through the C runtime thunks, whose FILE
handles and name strings are emulated memory), so the save image reaches them as the guest
address of `G.save`; `state_save_image()` (state_view.c) is the explicit serializer the
checkpoint uses. Calling it from the file layer has to wait for R4, when the C runtime thunks go
and a file function takes a pointer and a size. `NANPA2_SAVETEST`, the `saveload` golden and
`replay_det` (FLAG files compared between runs) cover the round trip.

**Out-of-range indices.** The native code does not range-check script-supplied numbers: an
animation slot, an argument index, a surface number, a cursor frame, a sprite pattern, a tile
number, a history entry. Because the objects are laid out as natively and contiguously, such an
index reads or writes the bytes it did natively, now inside `G`: a slot past 9 runs on into
`last_frame`, `idle` and the map cells; a surface number past 3 reads the pitch table and then
segment 1; a tile offset past 1000 reads the history ring; an offset in a segment past 64 KB
enters the next segment. This is why those accesses are not turned into bounds-checked array
accesses: they would change behaviour (a script that does it is undefined in the original, but a
replayed run must stay identical). Where converted code indexes by such a number
(`cursor_plane`, `hist_*`), it addresses the member by byte offset and may leave it, and the
`_Static_assert`ed layout says what it lands in. An access that leaves `G` altogether (below
0x431000 or from 0x4c0000 up) is the emulated memory again, and one that straddles the edge
traps, as before.

**Still addressed, on purpose.**

1. *Pointers stored in state.* The script segments' bases (`G.save.n.segment[]`, saved in the
   FLAG image), the program counter and label offsets (offsets from 0x435e78), the animation
   slots' `start` and `pc` (addresses in the ANI file), the locked pixel pointers, the surface
   and DirectDraw objects (`G.rt.surface`, `G.sc.scratch_surface`, `ddraw`, `palette_obj`) and
   `G.rt.top_esp` hold emulated addresses. They are values of the original's address model;
   turning them into indices or C pointers changes the saved image (segments) or needs R2 (ESP,
   the registers that carry them) and R4 (the DirectDraw objects).
2. *The register-style map code.* map*.c, anim.c, ops_*.c, wipe.c and screen.c still carry the
   native registers (`cpu.r[ESI]` = the sprite, `edi` = a cell) and read through addresses
   computed from them: `rd16(sp + S_X)`, the cell and shadow grids (`edi = CELLS`),
   the picture buffers. The bytes are in `G` (the alias) and the address arithmetic is the
   original's; removing it is R2's job, not a state move.
3. *The code, .rdata, the stack, the heap.* `x86_mem` outside 0x431000-0x4c0000 holds the exe's
   code (decoded by the interpreter), the read-only data (tables behind the C runtime and
   the DirectDraw thunks), the emulated stack (R2) and the heap: the C runtime's FILE handles,
   DirectDraw and DirectSound surface and COM objects, `malloc`ed buffers, the surfaces' pixels
   (R4: direct platform calls remove them). `raw.heap` and `raw.stack` still cover them.
4. *Read-only tables.* `head` (strings and formats) and `map.v.tile_offsets` /`subop` are asset
   data that the game never writes (the data check proves it for the runs above); they are
   initialised from the asset at start-up and kept in `G` only because they sit between
   mutable members. Decision: they stay as asset-backed tables, never copied into the source.
5. *`x86_icount`* (R3) and the CPU registers (R2).

**Boundary after R1**, precisely: game state is `G`, a C object. `x86_mem` is used for the exe's
code and read-only data, the stack, and the heap objects of the HLE layer; the aliases are how
the C runtime thunks, the HLE layer, bulk copies and the script VM's segment accesses see `G`.
Nothing outside `src/game`, `src/hle` and `src/x86` names `G`.

## Tooling after R1

| Tool | State |
|---|---|
| ctest `no_game_bytes`, `asset_only`, `wavcheck`, `selftest`, `start_trace`, `replay_det` | kept, pass (R2 removed `start_trace`) |
| golden replays | the check for every chunk |
| differential harness (`NANPA2_DIFF`, `diff_replay`, `NANPA2_DIFF_DIR`) | **frozen** at b98343c: it snapshots and compares `x86_mem` directly, which no longer holds aliased state, and the C is no longer meant to equal the lifted original. Not built into any test. |
| `NANPA2_FUZZ`, `map_fuzz.c`, `NANPA2_LOCKSTEP`, the recomp backend | **frozen**: they share the harness's direct memory access and need the exe; the sources stay for reference and compile, nothing exercises them. Revive by reading memory through `x86_read_virtual`. |
| `tools/reimpl_status.py` | unchanged (counts what the exe's code still contributes: nothing) |

## R2: ordinary C calls

**Pacing is unchanged in R2.** The game clock follows the instruction counter, so every unit keeps
its `X86_COUNT` charges and `X86_POLL` backward-jump polls, now spelled `COST(n)` and `POLL(at)`
(src/game/ctl.h): a plain cost counter that no longer depends on the CPU model. Changing them is R3.
That is why `li.*`, `raw.icount` and `raw.data` of the golden replays stay equal through all of
R2 (the goldens are in a scratch folder, recorded on d79f8e0: `golden_r2`).

### Control flow: direct style, the pump is a call

Options weighed: (a) an explicit state machine or resumable VM steps returning to a main loop that
pumps, (b) a coroutine or a second thread for the game logic, (c) the game calls a pump function
that returns a quit flag and a quit unwinds. **(c) was chosen.** Every wait in the game (the VM
loop's pass, frame waits, click waits, the menu loop, the wipe, the map's player step and event
check, the animation and fade waits) is a loop whose only interaction with the outside is the
message pump `ctl_pump()`; the platform layer already runs inside that pump on the same thread
(host events, timer callbacks, presentation), which SDL requires on the main thread on macOS. So:

- (a) would rewrite the hundreds of pieces that R2 removes into hundreds of explicit states, to
  gain a main loop that already exists inside the pump;
- (b) needs a context switch per wait: `ucontext` is deprecated on macOS and absent on Windows
  (fibers there), threads need the event handling on the main thread and a handshake per pump
  (and make the instruction-count clock nondeterministic unless strictly alternated): platform
  specific machinery for no gain;
- (c) is portable C (`setjmp`/`longjmp` is the one non-local exit), deterministic, and keeps the
  natural shape of the original: nested calls for nested blocks.

`ctl_quit()` (WM_QUIT, the end of the script at the top level, opcodes that end the VM) unwinds to the
innermost `ctl_catch`; a script restart (04 1e, 04 21 from a nested block) `longjmp`s to the top of the VM
loop. No frames between hold resources, so the unwinding needs no cleanup; it charges the three
instructions of the native stack reset and `pop ebp; ret`.

The script VM (src/game/loop.c, ops.c, ops_vm.c): `vm_pc` is the program counter (an offset in
segment 0, the native ESI), `vm_loop()` is the native loop head and returns the block result, a
nested block is a nested call of it (`vm_depth` replaces the native test ESP == [top ESP]), handlers
return -1 (the native `jmp 0x4060f9`) or the block result (the native `ret`).

### What moved off the CPU model, module by module

| Module | Before | After |
|---|---|---|
| VM loop, blocks, text, waits, all opcode handlers (loop.c, ops.c, ops_vm.c, ops_sys.c) | pieces returning native pcs, `call_exit`, `push32(ESI)`, EAX/ECX/ZF contracts, jump units | `vm_pc`, nested `vm_loop()`, handlers `int fn(void)` in two opcode tables, locals for what the native handler pushed |
| map (map*.c) | `cpu.r[ESI]` = sprite, `edi` = cell, `rd16(sp + S_X)`, pump chain `go(ra)` | typed `sprite`, `map_rec`, `cell` views over `G.seg1` / `G.map` (byte-offset indexing keeps the unchecked-index behaviour), movers return values, the chain is direct calls to `game_map_pump()` |
| graphics (screen.c, text.c, wipe.c) | cdecl words read from the emulated stack, results in EAX, wipe as pieces with a native frame | typed functions; pixel loops on `x86_ptr` pointers; `game_wipe`, `game_palette_wait` plain loops |
| anim, files, expressions, start-up (anim.c, file.c, expr.c, startup.c) | register conventions, WinMain as pieces, window procedure through `x86_call` | typed functions, `game_main`, `game_wndproc(hwnd, msg, wparam, lparam)` called by the pump |
| dispatch | `game_units[]` by native address, `hle_jump`, `HLE_GAME_*` lists, an interpreter | none: removed |

### What still uses the CPU model, and why

1. **The HLE boundary** (`hle_invoke` in ctl.c, the FILE/path buffers and COM/GDI scratch that
   file.c, startup.c and screen.c place on the emulated stack with `cpu.r[ESP]`): Win32,
   DirectDraw, DirectSound, GDI and C runtime thunks take their arguments from the emulated stack
   by native address and return EAX, and their objects (FILE handles, surfaces, COM objects) live
   on the emulated heap. R4 replaces them with direct calls.
2. **The emulated address space**: `G` is reached through an alias, `x86_ptr()` converts the guest
   addresses that remain in state (segment bases, animation `start`/`pc`, locked pixel pointers,
   `top_esp`, surface and DirectDraw objects) to host pointers. The *values* in `G` are kept
   identical because the FLAG image and `raw.data` need them; the code holds C pointers or offsets
   where it works on them (map, wipe, blits, picture). `top_esp` is written (as natively) but
   nothing reads it: it is dead state kept for `raw.data` until the goldens are re-recorded in R3.
3. **The instruction counter and the poll countdown** (`COST`, `POLL`, `x86_icount`): the game's
   clock, R3.
4. `cpu.r[ECX] = cpu.r[EDX] = 0` after the pump (`ctl_pump`), and `win32.c` saving the CPU around the
   window procedure, are what the HLE override did for the native pump; nothing in C reads them.

### Verification of R2, and which raw keys change

Every chunk was checked by the four goldens (`newgame` 271, `town` 301, `saveload` 33, `menu` 161
checkpoints, recorded on d79f8e0 with the same inputs as the R1 goldens) with
`NANPA2_REPLAY_CHECK=li,raw.icount,raw.data`: all `li.*` keys, the instruction count and the whole
data area equal at every checkpoint. `NANPA2_REPLAY_CHECK` now takes a comma-separated list of key
prefixes (`li` is every `li.*`). Expected to change in R2, and not compared: `raw.cpu` (registers
are no longer the state), `raw.stack` (the emulated stack holds only HLE argument frames) and
`raw.heap` (same), and `raw.pg` follows `raw.data`. `raw.icount` and `raw.data` are equal because pacing
and the data layout are unchanged; R3 changes the first, so after R3 the goldens are re-recorded.
`NANPA2_HLETRACE=2` prefixes each HLE call with the instruction count: with equal pacing two builds
give the same trace (checked on saveload and menu against d79f8e0 after dropping the lines of
calls that became direct C calls and the stack garbage that parameterless thunks print).

### Tooling after R2

| Tool | State |
|---|---|
| ctest `no_game_bytes`, `asset_only`, `wavcheck`, `selftest`, `replay_det` | kept, pass |
| ctest `start_trace` | removed: it compared the C start-up with the original WinMain run as an oracle |
| differential harness, fuzzers, lockstep, the recomp backend, `tools/recomp.py`, `tools/liveness.py`, `tools/reimpl_status.py`, the x86 interpreter | removed (nothing runs native game code; the units are no longer register-level functions an oracle could be compared with). `NANPA2_FROZEN_TESTS` is gone |
| golden replays | the check; `NANPA2_REPLAY_CHECK` selects keys |

## R3: pacing

**What it replaced.** Every unit charged the instruction count of the native code it replaced
(`COST(n)`, and `POLL(at)` for a backward jump, about 900 sites) and the game clock was that count at
25 MIPS. R3 removes all of it: no `COST`, `POLL`, `X86_COUNT`, `X86_POLL`, `x86_icount`, poll
countdown, `RT.top_esp` (the word stays in `struct vm_rt` as `unused_top_esp` for the layout), the
`ECX = EDX = 0` after the pump and its self-test, the CPU save around the window procedure, the
evaluator's instruction count, the `--cpu-mips` option.

### How the time model was derived

The charges were measured first: a profiling build attributed every charge to its function while
the four golden runs replayed (newgame 4320 s of game time, town 4800 s, saveload 128 s, menu 640 s).
Newgame, in seconds of the 4320 (25 MIPS):

| Where | s | Share | What it is |
|---|---|---|---|
| `win32_pump` (500 instructions a call, 193 million calls) | 3854 | 89.2 % | the spin of a wait on the 16 ms timer: not work |
| waits' own loop bodies (`vm_wait_frames`, `vm_input_down`, `game_anim_step`, ...) | 134 | 3.1 % | the same spin: 3.5 % on top of the pump |
| `vm_eval` (229 instructions a call, 17.7 million calls) | 162 | 3.8 % | script expressions |
| `vm_skip_block` (4 to 6 instructions a byte) | 49 | 1.1 % | skipping a block |
| `TextOutA` (5000 instructions a byte) | 49 | 1.1 % | GDI text, 0.2 ms a glyph |
| decompression (70 per input byte) | 22 | 0.5 % | GP4 and ANI files |
| opcode fetch, argument parsing, handlers | about 30 | 0.7 % | about 2 µs an opcode |
| `game_draw_char` cell copy | 12 | 0.3 % | 8 instructions a pixel |
| blits (`blit_cost`) | 4 | 0.1 % | |
| everything in the map, animation steps, cursor, palette, fills | under 3 | under 0.1 % | |

The transitions are timer-paced; the busy work matters only in the few places where the game runs
between two timer waits. The model (`src/game/pace.h`, each constant an instruction cost / 25) charges
the pump (21 µs: 500 instructions plus its share of the wait loop), the opcode (2 µs), the expression
(9 µs), the block skip (1 µs + 0.2 µs a byte), the GDI glyph (200 µs a byte), the character cell
(n*n/3 µs), the GP4 decode (70 instructions a compressed byte, 1.2 an output byte), the blit (pixels /
100 µs plain, / 25 keyed), the dissolve pass (25 rows of 260 µs + 6.4 µs a set mask bit, so 7.2 ms on average, 128 passes), the fade
set-up (290 µs) and one round of the two waits that do not pump (the animation slot wait and the
fade wait: 1 µs). Applied beside the old charges on the four golden replays the model's game time was
newgame 4371 s against 4320 (+1.2 %), town 4867 against 4800 (+1.4 %), saveload 126.3 against 128.0
(-1.3 %), menu 639.4 against 640 (-0.1 %).

The map's busy work (about 2 s of 4800), the cursor, the animation steps, palette work and fills have
no cost: the walk is paced by its 3-frame wait (48 ms a step before and after).

### The clock

`src/hle/clock.c` keeps the virtual machine time in microseconds, advanced by `ctl_work_us(us)` (game
code) and one `PACE_PUMP_US` per pump. `ctl_work_us` also keeps the host up: it throttles, runs the timer
callback if it is due, and presents, services the status file and SDL events at most every 8 ms.
The 16 ms timer callback, the game's clock, runs against one of two sources:

- **real time x speed** (default, speed 1.0): the game runs at the original's speed. The work costs are
  made real by sleeping when the virtual time is ahead of the host's (the old throttle, with the
  budget in virtual microseconds). `--speed X` / `NANPA2_SPEED=X` scales both; `NANPA2_TURBO=N` is
  the old name.
- **the virtual time**: deterministic mode (`--record`, `--replay`, `NANPA2_DETERMINISTIC`) and speed 0
  (as fast as the host can; replays use it unless `--speed` is given). Nothing reads a host clock,
  so the game's behaviour is a function of its input, as before.

A recording is format 2: `nanpa2-replay 2`, `pace N` (`PACE_VERSION`, bumped when a constant changes,
since a recording depends on the model), and `raw.clock` (the virtual microseconds) replaces
`raw.icount`. `NANPA2_REPLAY_CHECK=none` replays the messages without comparing checkpoints (recorded
input on a build with other timing).

### Measured before and after

The game's own clock in ms (`NANPA2_TIMELOG=1`, one line per fade, wipe, character, step and file
open; `tools/timing_stats.py`), the same scripted input on 90e7de9 and on the R3 build in deterministic
mode, which is real seconds at speed 1.0 (and checked in real time, below). The scenario is the load
path of the saveload golden, `NANPA2_INPUT` events replayed to tick 794, then no button, or a button
held from tick 800:

| What | 90e7de9 | R3 |
|---|---|---|
| palette fade (16 ticks) | 258-272 ms, median 267 | 258-272 ms, median 268 |
| dissolve wipe, no button | 4094 and 4096 ms | 4094 and 4096 ms |
| dissolve wipe, button held | 949 ms | 957 ms |
| title (start1.mes) to the load's open1.mes | 2312 ms | 2329 ms |
| first line of OPEN1, 8 characters (TextOut + cell, no delay) | 4 ms | 5 ms |
| a text window of 40 or more characters (bot runs, 1896 against 2242 windows) | median 709 ms | median 703 ms |
| a step of the player in the town (a screen-width walk is its step count x 48 ms) | 48 ms (3 frames) | 48 ms |
| whole load scenario to the first wipe | 12811 ms | 12798 ms |

Title to prologue: the load path above (title to OPEN1) is the measurable one. A new game's name entry
is the player's (the bot took 17296, 28551 and 20336 frames to reach S1.MES in three runs), so there is no
fixed number to compare.

In real time (non-deterministic, headless, the same script, host clock): 90e7de9 (its own 25 MIPS throttle)
had the first dissolve at 4098 ms and title to open1.mes at 2297 ms; R3 at `NANPA2_SPEED=1` 4094 ms and
2312 ms, and 18.5 s of game time in 18.7 s (0.98x including the 0.2 s start-up). At `NANPA2_SPEED=4` the
same script ran 18.4 s of game time in 4.9 s (3.78x, dissolve 1024 ms), at 8 in 2.6 s (7.11x: the host's
sleep granularity and the pumps' overhead show), with no warning.

Save images: no comparable run writes one on both builds (saveload writes none, and the old inputs
desynchronise before the Prologue's saves). The evidence that the FLAG files are unchanged is the diff
review below and `selftest`'s save round trip; the `golden_r3` newgame and town runs write FLAG0 and FLAG6
(replayed twice with equal `li.save` and `li.flags`).

### Determinism and the goldens

`golden_r3` (kept outside the repository) was recorded on the R3 build with the old bot seeds for
newgame (seed 31, 270000 frames: title, name entry, the Prologue, OPEN1, 70 MES files) and saveload (seed
41, 8000 frames: the load into OPEN1), which still cover the same ground, and with new seeds for the other
two, because the bot's choices depend on pump counts and the old seeds no longer reach the town or the
S46-S53 menus: town (seed 48, 330000 frames: TOWN.MES at
frame 135623, 39.MES, IZUMI, PARA_SUB, the player walking by mouse) and menu (seed 46, 40000 frames: S46-S53). Each replays twice,
every checkpoint matching with all keys (`li.*`, `raw.clock`, `raw.data`, `raw.heap`, ...), the two
replays recording identical files; `NANPA2_DATA_CHECK` reports 0 ranges. `replay_det` (ctest) now
records twice, compares the recordings and the FLAG files, replays, and checks the two tampered recordings
diverge.

### Content, not timing

`tools/replay_diff.py --sequence A.rec B.rec [--logs A.log B.log]` compares two recordings by what the
game went through rather than when: the cumulative MES chain (`li.mes`), the distinct FLAG states, the
quiescent screens as a longest common subsequence, and the names of the MES files from `NANPA2_TIMELOG`
logs. Result against the golden_r2 runs:

- saveload (same bot seed, same path): the 3 MES files, the FLAG state, all 3 quiescent screens and the
  last screen and palette are identical.
- The old recordings' messages replayed on the R3 build (`NANPA2_REPLAY_CHECK=none`) stay in step for the
  first 12 MES files in newgame and town (title, name entry, S1 on) and 9 in menu, then desynchronise:
  the messages are at tick and pump, and the pump counts of a wait differ by a few percent. That is the
  limit of comparing by input.
- The exploration bot makes its choices from the pump count, so its long runs take other paths with
  the new clock (newgame, same seed, shares the first 16 MES files with the old run and then differs, for
  example S10 -> S12 where the old took S10 -> S11). Over the four sets the R3 runs open 90 distinct MES
  files and 137 transitions, the old 77 and 112. These runs are different plays, so they can neither
  show nor rule out a content difference.
- The charge removal itself was reviewed as a diff: about 900 deleted statements, all whole-line `COST`,
  `POLL`, `X86_COUNT`, `X86_POLL` (none with a side effect in its argument), and 18 `if (cond)` lines whose only body was a
  deleted charge. No other line of the game logic changed, except the animation slot wait and the
  fade wait, which gained `ctl_work_us(PACE_SPIN_US)`: they spin without a pump on a flag the timer
  callback sets, and the timer used to run from the polls they no longer have.

### Left for R4 and later (all done in R4 except the map code's indexing, which stays on purpose)

- `hle_invoke`, the C runtime and Win32 thunks, the emulated stack, heap and address space (R4).
- The `native_pc` and `exit_pc`/`loop_pc` parameters of the opcode helpers in ops.c, ops_sys.c and
  ops_vm.c are documentation of native jump targets that nothing uses now; remove with the opcode
  tables' native addresses.
- `RT.unused_top_esp` and the other native words nothing reads (`raw.data` covers them).
- The map code still carries the byte-offset indexing for the unchecked-index behaviour (R1).
- A per-opcode cost proportional to the expression's length (the evaluator's 229 instructions average
  hides a range 6 to 200) if the script loops ever feel off; the model's constants are in one file.

### Left for R4

- **R4**: replace `hle_invoke` and the thunks by direct calls (FILE handles as `FILE *`, surfaces as
  platform objects, COM objects gone), then the emulated stack, heap and address space can go: `G`
  stops being aliased and its pointer-valued fields (segment bases, animation `start`/`pc`,
  locked pixel pointers) become indices or C pointers with conversion at the FLAG save and load.
  R3's leftovers are listed at the end of the R3 section.

## R4: a free-standing C engine

R4 removed the last of the model of the PC: the Win32, DirectDraw, DirectSound and C runtime thunks
(`hle_invoke`, `HLE()`, the import table and the COM vtables), the emulated stack, heap and 32-bit
address space (`src/x86`), the aliases of `G`, and the checkpoint keys of the emulated machine. The
game logic now calls a small platform API, and `G` is an ordinary C object. Commits (oldest first,
on top of 5b37d43): 8123dcd (the checkpoint key `raw.gdata`), aee4867 (files and sound),
b168ef3 (surfaces, text, window, timer: `hle_invoke` and the thunks go), 5d5612e (`src/x86`,
the FLAG serializer, the dead parameters), f40feb6 (`src/hle` dissolved into `src/platform` and
`src/dev`), b55d64f (comments, includes, build scripts), f1bd5f8 (self-test), and the documentation
commits after them.

### The checkpoint keys, and why the recordings were re-recorded first

`raw.data` hashed the emulated data area, which held the surface handles, the DirectDraw lock
descriptor and the locked pixel pointers: addresses of the emulated heap. Once surfaces are platform
objects those words cannot keep their values, so before anything else changed (8123dcd, on the R3
code) a new key was added beside the old ones: **`raw.gdata`**, the hash of the whole of `G` with the
fields that hold platform values zeroed (`platform_fields[]` in `src/game/state_view.c`: `rt.surface`,
`sc.scratch_surface`, `hwnd`, `hinstance`, `font`, `surface_desc`, `lock_ptr`, `surface_desc_rest`,
`ddraw`, `palette_obj`, `timer_id`, and the three locked pointers of `map.v`; the pitches, the segment
bases, the animation addresses and every other word stay in). That commit also made `--replay` with
`--record` write the replayed messages, so a golden can be carried over to a new key set by replaying
it. The procedure: replay the four `golden_r3` recordings on 5b37d43 + that commit, with
`--record`: every old key matched (`replay: ok` for all four), the new recordings' messages equal the old
ones and their `li.*` and `raw.clock` equal too (`replay_diff.py --li` says "identical" between a
`golden_r3` and a `golden_r4` recording), and they carry `raw.gdata` as well. These are `golden_r4`
(kept outside the repository, with the old keys still in the files). Every later commit replays all
four of them.

| Key | Now |
|---|---|
| `li.frame`, `li.screen`, `li.pal`, `li.save`, `li.mes`, `li.files`, `li.vm`, `li.flags` | unchanged; `li.save` is the export of the FLAG serializer |
| `raw.clock` | unchanged (the virtual microseconds) |
| `raw.gdata` | new: hash of `G` with the platform words zeroed; same value in R3 and R4 code |
| `raw.cpu`, `raw.data`, `raw.heap`, `raw.stack`, `raw.pg` | retired with the emulated machine. A recording that has them still replays: the keys the live run does not produce are skipped (`replay.c`, `replay_diff.py`) |

The recording header stays `nanpa2-replay 2`, `pace 1`: the format of the file (messages and key=value
checkpoints) did not change, only the set of keys, and a reader takes the keys of the recording; an R4
recording is recognised by the absence of the retired keys (and the presence of `raw.gdata`).

`NANPA2_DATA_CHECK` (a snapshot of the data area outside the aliases) is retired: there are no
aliases and nothing outside `G`; an access that leaves `G` traps (below). `replay_det` no longer runs it.

### The platform API

Game code calls these directly (no thunks, no argument frames, no emulated memory); the host
implements them in `src/platform`.

| Header | What the game uses | Replaces |
|---|---|---|
| `platform/files.h` | `files_open(path, mode)` with the game's paths (`DATA\x`, `Q:\DATA\x`, `FLAGn`), `files_size`, `files_drive_is_cd`; the save folder, case rules and the open log are inside | fopen/fread/fwrite/fclose/malloc/sprintf/strrchr/_stricmp... of `crt.c`, GetDriveTypeA |
| `platform/surface.h` | `surf_create`, `surf_lock` (pixels and pitch), `surf_size`, `surf_set_colorkey`, `surf_has_colorkey`, `surf_blt_fast`, `surf_fill`, `pal_set`, `screen_snapshot`, `screen_present`, `screen_wait_vblank` | DirectDraw objects, Lock/Unlock, BltFast/Blt, SetEntries, WaitForVerticalBlank |
| `platform/text.h` | `text_init`, `text_font_create`, `text_draw(surface, x, y, font, bytes, len, fg, bg, opaque)` | GDI: CreateFont, GetDC, SelectObject, SetTextColor/BkColor/BkMode, TextOut |
| `platform/window.h` | `win_open(handlers)`, `win_show`, `win_post`, `win_pump`, `win_timer_start/stop`, `win_show_cursor`, `win_warp_cursor`, `win_message_box`; WM_* numbers | RegisterClass/CreateWindow/ShowWindow, PostMessage, PeekMessage, timeSetEvent, ShowCursor, SetCursorPos, MessageBox |
| `platform/sound.h` | `sound_init/shutdown`, `sound_music_play/stop/pause`, `sound_voice_play/stop` | the sound overrides called by native address |
| `platform/logo.h`, `platform/clock.h` | `logo_play`, `clock_*` (pacing, R3) | the logo override |

Choices: surfaces are named by 32-bit **handles** (kept in `G.rt.surface[]`, so the script's unchecked
surface numbers still index words of `G`), with their pixels in plain memory and `surf_lock` returning
the pointer (the lock is a formality); every surface has `SURF_SLACK` (1 MiB) of slack each side, because the
original ran over the end of a surface into the next heap block when a script's rectangle was out of
range, and a write that lands in the slack is harmless. The primary surface's handle is a constant
(`SURF_PRIMARY_HANDLE`, the value the emulated heap gave it): the script register S receives the last
unlock's result, which is the screen's handle after an off-screen surface was unlocked, and it is in the
saved image. Costs of the time model (`ctl_work_us`) are charged by the game at the points they were
charged before (blit, fill, glyph, decode), not by the platform. The Win32-message emulation stays in
`platform/window.c` as an implementation detail: the host's events become WM_* messages in a queue and
`win_pump` hands one to the game's window procedure (`game_wndproc`), because the scripts depend on
Windows' way of delivering input (button down and up as separate messages, key repeat dropped, mouse
moves coalesced, input as bits that a click handler and a wait loop poll). The timer tick
(`game_timer_tick`, native 0x4028c0) is game code in `src/game/timer.c`; the platform calls it.

### G addresses, and pointer-valued state

`G` stays one contiguous object in the native layout. The values the game keeps in it that the
original used as addresses are kept as they were, as **G addresses**: integers in the original's numbering
(0x431000-0x4c0000) that index `G` (`g_ptr(a, n)` in `src/game/gmem.h` is `(uint8_t *)&G + (a - 0x431000)` after a
bounds check). They are not host pointers and nothing in `G` is. The reason is the unchecked indices
("Out-of-range indices", above): a slot number past 9, a surface number past 3, a tile offset
past 1000 or an offset past the end of a segment read and write the bytes after the member, which needs `G` to be
contiguous and an address to be a position in it. The numbering is the original's because the FLAG
file holds the segment bases and the data hash covers them. What each address-valued word became:

| State | Held | Now |
|---|---|---|
| script segment bases `G.save.n.segment[4]`, the VM's program counter and labels (offsets), string and block arguments | in the FLAG image / G | G addresses and offsets, unchanged; the serializer does not take the segment bases from a file (below) |
| animation slot `start`, `pc`, `loop[2]` (addresses in the ANI file) | G | G addresses, unchanged (`anim.c`) |
| the locked pixel pointers (`G.map.v.locked_*`, `G.sc.lock_ptr`) and the lock descriptor | G, emulated heap addresses | C pointers in `map_draw.c`'s `lk` and local variables; the G words are unused (kept for the layout) |
| surface and palette objects, DirectDraw object, window, font and timer handles | G, emulated heap addresses | platform handles for surfaces and fonts (`G.rt.surface`, `G.sc.scratch_surface`, `G.sc.font`); the rest unused |
| `G.rt` word at 0x445cdc (the native top ESP) | G | `pad_cdc`: outside the FLAG image, dead since R3 |

An access that leaves `G` used to fall through to the emulated memory (zeros, read-only data, the heap, the
stack); it now traps (`game_fault`, "game memory fault"), as does a file that would run past the end of
the data (`game_file_load` knows the room it has), an ANI/GP4 stream that does not decode, a surface
number that names no surface, a script opcode outside the native tables and the other
cases that were `x86_trap` (now `game_trap`, which prints the native address of the code that would
have run, the frame, the script and the script offset). `game_divide_error` is the expression
divide by zero.

### The FLAG serializer

`state_save_export` and `state_save_import` (`src/game/state_view.c`) are the one place the state is
converted to and from the file. The file is `G.save`, 0xc00 bytes, in the original's layout (all
little-endian, packed, each field `_Static_assert`ed at its offset): the running script's name (32 bytes, 0x0),
the 4-bit flags (0x400 bytes, 0x20, two per byte), registers `@`..`Z` (27 x u16, 0x420), the word array `@[0..]`
from 0x456 with the named words of `struct save_image` (map selector, surface select, segment select, text
delay and flags, mouse, the text window and cursor, colour, size and advance, the menu list pointer, rows and
count, the 16 base palette words at 0x494, the four segment bases at 0x4c4, the last picture's rectangle at
0x512, then the work buffer to the end). Fields that hold addresses: **only the four u32 segment bases**; every
other field is a value. Export writes the bases as the original's addresses; import takes everything from the
file except the bases, which the VM set-up that follows every load (`vm_init`) derives again, so an old
or hand-made file cannot put a bad address into the engine. A short file leaves the rest of the state as it was
(as the original's read did); a longer one is read up to 0xc00 bytes (the original overran into the words after the
image, undefined). The op `04 21` loads and saves through them (`ops_vm.c`), and the checkpoint `li.save` and
`NANPA2_SAVETEST` use the export.

### Verification

- **Goldens**: the four `golden_r4` recordings (newgame 271 checkpoints, town 331, saveload 33, menu 161) replay
  with every checkpoint matched on `li.*`, `raw.clock` and `raw.gdata` after every chunk (files, sound,
  surfaces/text/window, `src/x86`, the directory move) and on the final commit twice (`fin1`, `fin2`), headless, scratch
  save folders.
- **Saves are byte-compatible**: the replays of newgame and town write FLAG0 and FLAG6 during the run (Prologue
  saves); the files the final build wrote are byte-identical (3072 bytes each, same `cksum`) to those 5b37d43 wrote for the
  same recorded input, for all four goldens. The install's own `FLAG0..6` (copied; `game/` untouched) were loaded by a bot
  run recorded on 5b37d43 (seed 41, 8000 frames: the title's Main item reads `flag6`, then OPEN1) and replayed on the
  final build: 33 checkpoints matched (`li.save`, `li.files`, `li.mes` included), all seven files unchanged. By
  construction the format is the old one (the layout asserts did not change) and the original's own load code is
  the format's definition, as before.
- **Long run**: 300000 frames of the bot (seed 48, deterministic, speed 0, from the town saves): 4800.3 s of game time
  in 45 s, TOWN.MES opened 7 times, 513 file opens, no trap and no warning; the 5b37d43 binary on the same run opens the
  same 513 files in the same order and writes identical FLAG0 and FLAG6.
- **ctest**: selftest (now through the serializer), replay_det (5.6 s instead of about 35: the engine is faster
  without the emulated stack), wavcheck, no_game_bytes, asset_only (recordings of an asset run and a whole-exe run
  must be equal), smoke (skipped: no visible screen).

### Portability audit

The code assumes a **64-bit little-endian host**; `state.h` has an `#error` for other byte orders, and no code
depends on the width of `long`, `size_t` or a pointer (`uint32_t` for G addresses, `uint8_t *` for pixels, `size_t`
for sizes). What was checked: a syntax-only compile of every source with `-Wshorten-64-to-32 -Wpointer-to-int-cast
-Wint-to-pointer-cast -Wcast-align -Wformat=2 -Wsign-compare` (the former `guest()` casts of pointers to
`uint32_t` are gone with the emulated memory; the remaining narrowing is intentional 16- and 8-bit register and
field arithmetic of the scripts, plus minimp3's own `size_t` to `int`; the one alignment cast of game data, the
palette copy, now goes through a local array); the packed structs (`#pragma pack(1)` with the offsets
asserted) are never pointed into by address-of except the anim slot, which is packed too, so unaligned members are
accessed by the compiler's packed access; the `-m64` question is moot on this machine (arm64 is LP64) and the Windows
LLP64 difference does not touch the game (`long` appears only in `ftell` and file sizes, and in the presenter's counters). Not available here: a 32-bit,
big-endian or Windows build; `tools/build_msys2.sh` stays untested. `getline` (replay.c), `__typeof__` and
`__builtin_expect` are GNU/POSIX (MSYS2 GCC has them, MSVC does not).

### What the platform's behaviour changed (and what did not)

Not changed: every observable of the goldens. Changed, all in cases the original left undefined: a script
that writes outside `G` or a file that runs past the data now stops with a trap instead of writing into
whatever lay behind; a surface rectangle that leaves the surface by more than the slack
would fault; the order of the unchecked-index behaviours inside `G` is as before. Retired
dev variables: `NANPA2_DATA_CHECK`, `NANPA2_HLETRACE` (the HLE call trace),
`NANPA2_IMAGE_TRACE` (which bytes of the exe the run touched) and `NANPA2_INTERP` (now
`NANPA2_LOAD_EXE=1`: the data from the whole exe instead of nanpa2.dat, for the asset test).

The asset shrank after R4 (asset version 2): `state_init` is its only consumer and reads only
0x431000-0x4c0000, and `G` traps below 0x431000, so the PE header page and .rdata (82 KB) left it; it
holds the initialized .data only (12,170 bytes in 18 ranges, about 13 KB stored). Poisoning the image
outside `G` before `state_init` left all four goldens matching. Version 1 files are still read.

**G from code.** Of the 12,170 non-zero bytes of the initialised .data, `state_defaults`
(src/game/state.c) now defines what the engine reads, as typed fields: the CD prefix `"A:\\"`
(0x431030, `data_head.cd_prefix`), the animation slots' start 0xf4, the idle value 0xff and the script
random seed 0x8801 (0x4312cc-0x4313c4), the map's last_sprite, last_click and last_area 0xffff, the
sprite random seed 0x8a, the tile offset table (derived: 40 tiles of 2 bytes per row, rows 1280 bytes
apart; 0x4355fa), the script name `start.mes` and its DOS terminator bytes (0x444e78), the menu input
mask 0x7e, text window right 79 and bottom 399, text colour 7, line height 16, advance 2 (0x4452e1), the
word 0xffffffff at 0x445308, the 16 base colours (derived, 0x44530c), the FLAG name `flag0`
(0x445d86) and the fade length 8 (0x44628e). Not reproduced because nothing reads it: the native
function pointers (0x43100c-, 0x435576-0x4355fa, 0x435dfc-0x435e60, 0x4313d9), the native file-name
format, MCI and error strings of the head (0x431034-0x4312cc: the C holds its own formats), and 6,586
bytes at 0x4a72c0-0x4aac42 (the MP3 library's tables inside the ANI buffer, overwritten when the file is
read). Left in the asset (asset version 3, 124 bytes): the no-CD message and title (0x431140, 0x431158) and
the 7-byte text at 0x445c71, which are the game's own text. Check: with the whole image loaded and the
defaults applied, the bytes of G differ from the image only in the ranges listed as unread; the four
`golden_r4` replays match every `li.*` and `raw.clock` checkpoint on a build that starts from zero for
those ranges. `raw.gdata` of the old recordings no longer matches (it hashed the unread bytes), so the
goldens need `NANPA2_REPLAY_CHECK=li,raw.clock` from here on or a new recording.

### Architecture summary (R4 and after)

```
state_defaults (code) + nanpa2.dat (game text only) --state_init--> G (struct game_state, native layout)
                                                            |
 SDL events --window.c--> WM_* queue --win_pump--> game_wndproc --> G.rt.input_bits, cursor ...
 16 ms timer (clock.c) ---------------------------> game_timer_tick --> G.rt.frame, fades
                                                            |
 src/game: script VM (loop.c ops*.c vm.c expr.c), text, anim, map*.c, screen.c, wipe.c, startup.c, file.c
        |  reads and writes G by field names and G addresses (gmem.h)
        |  calls the platform API: files.h surface.h text.h window.h sound.h  (and ctl_work_us for time)
        v
 src/platform: files.c surface.c text.c window.c sound.c logo.c clock.c present_*.{c,m} -> SDL2 (+ Metal)
 src/dev: replay.c (checkpoints over G and the screen), explore.c, dev.c, status.c
```

Control flow is direct style (R2): waits are loops around `ctl_pump()`, a quit unwinds with `setjmp`/`longjmp`
to `ctl_catch`. Time is the explicit model of `pace.h` (R3). State is `G` and nothing else; the platform's
objects (surfaces, palette, fonts, window, audio) hold no game state the game reads except through the handles and
pointers it was given. The checkpoints hash the screen, the palette, the FLAG image, the files and scripts reached,
the game clock and `G` (platform words masked).

### Left

- `platform/files.c` includes `dev/dev.h` and `dev/replay.h` (the open hooks: the dev log and the checkpoint's
  file list), a dependency of the platform on the dev layer that a callback registration would remove.
- `SURF_PRIMARY_HANDLE` (0x0104b098) is an artefact of the old emulated heap, frozen on purpose: the value is in
  the saved image through register S (above). A new value would be harmless to the game but would change `li.save`
  of every recording and the saves written from now on.
- The map code still indexes its members by byte offset (`seg1_ptr`, `cursor_plane`, `hist_*`) to keep the
  unchecked-index behaviour; that is the contract of `G` and is documented where it is used.
- `G` has words nothing reads or writes any more (the DirectDraw lock descriptor, `ddraw`, `palette_obj`,
  `timer_id`, `hwnd`, `hinstance`, the three `locked_*`, `pad_cdc`): they are kept for the layout, marked in
  `state.h`, and masked in the data hash.
- Untested: the Linux and Windows builds, a 32-bit or big-endian host.
- `docs/reimpl.md` is the historical inventory of the 142 native functions.
