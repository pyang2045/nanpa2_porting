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
| R1 | game state in C structs instead of emulated memory | done: all of the data area is one C object; what stays addressed and why is listed below |
| R2 | ordinary C calls instead of register and stack conventions (`cpu.r[]`, `push32`, `go(ra)`, jump units) | done: see "R2" below |
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

### Left for R3 and R4

- **R3**: replace `COST`/`POLL` and the instruction-count clock by real-time pacing with the original's
  speed (the timer callback, frame waits, fades, text delay as time); re-record the goldens; drop
  `x86_icount`, `raw.icount`, the poll countdown and the 25 MIPS model; then `top_esp` and the `ECX/EDX`
  zeroing go.
- **R4**: replace `hle_invoke` and the thunks by direct calls (FILE handles as `FILE *`, surfaces as
  platform objects, COM objects gone), then the emulated stack, heap and address space can go: `G`
  stops being aliased and its pointer-valued fields (segment bases, animation `start`/`pc`,
  locked pixel pointers) become indices or C pointers with conversion at the FLAG save and load.
