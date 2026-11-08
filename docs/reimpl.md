# Reimplementing the game logic

The port runs the game code of `nanpa2.exe` in an interpreter (`src/x86/interp.c`, the default)
or as a static recompile (`tools/recomp.py`, development and reference). The
goal is an engine that needs only the game's data files: every game function written as C
from an understanding of what it does, until neither `nanpa2.exe` nor the generated
`recomp.c` is needed. This file is the inventory, the plan and the test method.
`tools/reimpl_status.py` measures progress and reads the module column of the inventory
table below.

## What is game code

`tools/recomp.py` lifts 0x401000-0x407800. Walked from WinMain (0x402090), the window
procedures (0x4021c0, 0x401fd0) and the code pointers in .data, the reachable game code is
**25,597 bytes in 142 functions, 0x401000-0x4077ac**. The rest of `.text` is not game logic
and is not lifted:

- 0x4077b0-0x412c81: a statically linked MP3 decoder library (filterbank, Huffman tables,
  0x409550 decode, 0x409f40 free). Only the sound functions call it, and those are
  replaced (`src/hle/sound.c` uses minimp3), so the library is already gone.
- 0x412c90-: the MSVC runtime (stdio, malloc, string, `_ftol`, startup). Calls into it go
  to `src/hle/crt.c` by address.

So CLAUDE.md's "game code is 0x401000-0x412210" includes the MP3 library; the real game
code ends at 0x4077ac (the last two functions are the DirectDrawCreate/DirectSoundCreate
import thunks).

A function is a direct call target or an extra entry. It owns the instructions reached
from its entry through fallthrough, jumps and jump tables, not calls, stopping at other
entries. An entry that the code before it falls into is an alternate entry of that
function (0x4039f7's tails 0x403a06/0x403a12/0x403a22/0x403a42, the VM loop 0x4060f9).
So the script VM 0x4060d4 owns all opcode handlers reached through its jump tables
(3,765 bytes). About a third of the functions are hand-written assembly with register
arguments.

## Stage 5B: measurement, palette, screen routines, spin pieces (src/game/screen.c)

**Measurement.** `tools/reimpl_status.py` used to count a function as replaced when nothing
reachable from the roots called it, so routines that the C still runs through `game_ncall`
counted as done. It now also takes as needed (a) every game address passed to `game_ncall` /
`ncall` / `blit` / `colour_rect` in `src/game/*.c` (or a `#define` of one) and (b) the original body
of every unit whose `native` predicate sends cases back to the native code (a body is walked
from its entry, stopping at the VM fetch 0x40616e). A handler with a native case counts in
full, so the figure is a lower bound for those. Stage 4's 50.4% became 36.9% at the first
commit of this stage. The interpreter counts the instructions it runs in 0x401000-0x4077ac
outside a differential oracle: `NANPA2_NATIVE_REPORT=1` prints the total and the functions
(nearest entry: a call target seen, a unit, an `x86_call`/`ncall` entry) at exit, and the
status file has `native_game_insns` with the top eight. A finished engine runs none. Spin
loops weigh most: in a 5000-frame run 68 M of 97 M were 0x403510 sub-op 4 waiting for a
frame and 25 M the fade wait of op 04 16 3.

**Spin pieces.** The compare holds the timer off (`x86_poll_countdown = INT_MAX`), so a C
loop that waits for the timer would hang under `NANPA2_DIFF`. A wait loop is a jump unit
(`SPIN` in units.c, `poll_self`) that does one round and returns its own address while it must
wait: `game_jump` then calls `X86_POLL` after the compare and the poison, once per round like the
native backward jump. The oracle of a piece stops when it reaches a jump unit again, so one
round is compared. Units: 0x4035b5 (slot control sub-op 4, entered from 0x403510, which is now
a jump unit entered by op 1a's call exit 0x4070b6, `JUMP_SYS_R` keeps the argument registers
live) and 0x406f41 (op 04 16 3 waiting for the fade flag 0x4ac504, which the HLE timer
callback 0x4028c0 clears). Native fallbacks removed: 1a sub-op 4, 04 16 mode 3.

**Palette** (`screen.c`): 0x40756c / 0x407599 save and restore the colour tables, 0x40754d
fill, 0x401ca0 upload (SetEntries through the vtable), 0x401d30 fade start, 0x401e10.

**Surfaces, cursor, rectangles** (`screen.c`): lock 0x401190, unlock 0x4012b0 and cursor
restore 0x4011f0 are units (native code, the map and the window procedure, calls them); cursor
draw 0x4012e0, show 0x401c40, hide 0x401c60, position 0x401c80, copy 0x401430, masked copy
0x4014f0, swap 0x401670, fill 0x4017b0, XOR 0x401870 and picture load 0x406e20 are called from
C only and compared as part of their callers. Their arguments are on the emulated stack at ESP
exactly as for the native callee (a unit finds them there after `hle_call`), the COM methods
go through the vtable with `game_ncall`, rectangles and DDBLTFX live in the native frame
addresses (a register that holds a frame address, such as EDX after the fill, would
otherwise differ), and the registers every routine leaves are the native ones: ECX's upper
half is compared for every unit, which found the cursor restore leaving its width in ECX (it
stores it over the saved ECX slot, `[esp+0xc]`).

**Not done.**

- The dissolve wipe 0x401a50 (op 19 sub-op 4: 128 passes, 2 frames apart, the pump between;
  needs pieces with the frame in emulated memory at the native addresses, and no explore run
  of 20000 frames reached it) and the click path 0x40613f-0x40616c (ext 20, never reached) stay
  native, with their predicates.
- File load/save, startup and the window procedure: see "Stage 5B: file, input, startup".
- Map: 04 26 sub-ops 5, 7, 12 and 0x403a70 belong to the map module (not touched). 0x40609c
  (module input, called only by the map code) is left to it too.

## Inventory by module

From `tools/reimpl_status.py` (2026-11-08, after stage 1):

| Module | Functions | Bytes | Replaced | What |
|---|---|---|---|---|
| map | 63 | 9,764 | 0 | town/school map: cell grid, sprites and their scripts, movement and collision, pathfinding (ext op 0x26, 0x403a70-0x40609c); many are handlers in .data pointer tables 0x435576 and 0x435dfc |
| vm | 10 | 4,295 | 0 | script VM loop and opcode handlers, block skip, argument parser, menus, waits |
| screen | 21 | 3,045 | 0 | surfaces, blits, cursor sprite, palettes, fades, the dissolve wipe, picture load |
| sound | 16 | 2,511 | 2,511 | DirectSound/MP3 music and voices (HLE: `src/hle/sound.c`) |
| startup | 8 | 2,236 | 779 | WinMain, window, CD search, DirectDraw/DirectSound init, timer, logo, shutdown |
| input | 5 | 956 | 75 | window procedure, message pump, input polling |
| file | 7 | 806 | 431 | file load/save, GP4/ANI decompressor (HLE: `src/lzh.c`) |
| anim | 4 | 687 | 0 | ten animation slots running ANI bytecode |
| text | 4 | 675 | 0 | character drawing (GDI), text cursor, number formatting, text window |
| expr | 4 | 622 | 622 | script expressions and flags (**stage 1, `src/game/expr.c`**) |
| **total** | **142** | **25,597** | **4,418 (17.3%)** | 27 functions (19.0%) |

Of the replaced bytes, 3,796 are HLE overrides of platform code (sound, timer, message
pump, decompressor, logo). They are hand-written, but they replace Windows/DirectX plumbing,
not game logic. **622 bytes (2.4%) are reimplemented game logic** (`src/game`). The startup
figure includes the timer callback 0x4028c0, which is owned by 0x402620 because it is
reached only as an immediate.

## Plan and order

The order follows dependencies: the VM core first, because everything else is reached
from script opcodes, then the subsystems the opcodes drive, from small and pure to large.

1. **expr, done.** Script expressions and flags: 0x4037fc evaluator, 0x4039f7 constant
   reader, 0x4039d6 flag read, 0x406455 flag write. They are pure, hot (millions of
   calls a playthrough) and called, so the call-override mechanism fits.
2. **vm helpers, done.** 0x4061d4 block skip, 0x40741b argument parser (+0x4074a3), 0x406dc4
   string argument, 0x406395 VM init, 0x4068e1 menu item step. These are called units with
   register arguments, mostly pure.
3. **Jump overrides, then the opcode handlers; done** except ext 0x20 (the mechanism and
   12 handlers in "Stage 2", the rest in "Stage 3" below). Opcode handlers are reached by
   `jmp [0x40620b + 4*op]` / `jmp [0x406262 + 4*xx]` and end with `jmp 0x4060f9`; they have
   no return address. Replacing them needs a second override kind: recomp emits
   `pc = hle_jump(addr); goto dispatch;` for the handler address, and the C handler returns
   the continuation pc (usually 0x4060f9). The differential harness needs an exit
   condition for the oracle: dispatch stops at a given (pc, esp) pair. Start with the
   assignments (ops 0x0a-0x0e), then 0x10-0x18 and the ext ops that only call already
   replaced code. Ops that run a nested VM (0x0f if, 0x13/0x14 call, ext 0x10 while, ext
   0x1f call MES) turned out not to need the loop in C: they are split at those calls
   ("call exits", Stage 3).
4. **The VM loop 0x4060d4/0x4060f9, done** ("Stage 4"; the click handler path stays
   native). Fetch, the 00/01/02 block structure (nested VM by
   native recursion and `mov esp, [0x445cdc]` unwinding), text output 06 and Big5 bytes,
   click handlers. After this the whole vm module is C, and the native stack tricks are
   gone.
5. **text and anim, done** ("Stage 4"). Character drawing 0x401900 goes through GDI imports (`src/hle/text.c`
   already rasterizes); the text cursor 0x4063ee, number formatting 0x4069fd, the text
   window 0x406b24. Animation slots 0x403510/0x403624/0x403661 (ANI bytecode).
6. **screen.** Blits 0x401430/0x4014f0/0x401670/0x4017b0/0x401870 and the surface lock
   0x401190/0x4012b0 (through `ddraw.c`), the cursor sprite, palette 0x401ca0/0x40752c/
   0x40754d-0x407599, fades 0x401d30, the wipe 0x401a50, picture load 0x406e20.
7. **map.** The largest module: cell grid and drawing (0x403b28-0x404281), sprite scripts
   and movement (0x4042f7-0x404e25), events (0x404f8c-0x405216), pathfinding
   (0x405597-0x405fb6, with the recursive flood fill 0x405b07 and its eight direction-order
   cases). Mostly pure computation on emulated memory, so it suits the harness well.
8. **startup, input, file.** WinMain, the window procedure, CD search and init/shutdown
   become plain C around SDL. File load 0x401050 and save 0x401160 go through the CRT
   replacements already. Then nothing is lifted any more: recomp.c and nanpa2.exe drop out,
   and state moves from emulated memory into C structs (a later stage).

## Override mechanism

- `HLE_GAME_OVERRIDE(addr, "what")` lines in `src/hle/overrides.h`, the list both backends
  read (the interpreter includes it, `tools/recomp.py` parses it into `GAME_OVERRIDES`, merged
  into `HLE_OVERRIDES`): calls to and dispatches into these addresses go to `hle_call`, which
  hands them to `game_call` (`src/game/game.c`) before the CRT and HLE switches. A new unit
  is one line there plus its entry in `src/game/units.c`.
- Unlike the HLE overrides, the lifted body of a game override is kept as the **oracle**: it
  is reachable only through the tagged entry `x86_exec(X86_ORACLE_TAG | addr)` (0x40000000
  | addr), never from game code. The interpreter implements the tag as "execute addr,
  skipping the override check for that entry" (and after direct jumps back to the entry from
  inside the body), so `NANPA2_DIFF` works on both backends. recomp refuses to build if any instruction outside a
  function's own body jumps to its entry, because that jump would bypass the override.
- A unit (`src/game/units.c`) is the address, the register-level entry point (the native
  calling convention: hle_call has popped the return address; arguments on the stack or in
  registers), the registers that are scratch by contract and whether a caller reads the
  flags. The C code below the register layer is ordinary C on typed accessors for the
  native globals (`src/game/vm.h`). Emulated memory stays the single state store.
- Replaced code charges an estimated native instruction count (`X86_COUNT`) so that the
  486 pacing (`--cpu-mips`) stays as it was. The diff run reports native and estimated
  instructions per call. The evaluator charges each token what the native loop runs for it
  (6 for the 03 end, 14 a digit, 23 a register, 27 most operators: `token_insns` in
  expr.c); over a 20k-frame explore run the C units' charges total 1.003 of the original's
  (Stage 3).

**Jump overrides (`HLE_GAME_JUMP`, opcode handlers).** A script opcode handler is entered
by the VM's `jmp [0x40620b + 4*op]` / `jmp [0x406262 + 4*xx]` and leaves by `jmp 0x4060f9`
(the loop head), `jmp 0x40638d` (VM end) or `ret` (ext 11/12 return from the block's VM);
there is no return address. So:

- `HLE_GAME_JUMP(addr, "what")` lines in `overrides.h` (recomp.py: `GAME_JUMP_OVERRIDES`,
  also in `HLE_OVERRIDES`; interpreter: `OVR_JUMP`). Reaching addr by any jump continues at
  `pc = hle_jump(addr)` (recomp: `case addr: pc = hle_jump(pc); goto dispatch;`;
  interpreter: at the override check of its fetch loop). `hle_jump` hands it to `game_jump`,
  which runs the unit's `jump` function: it reads and writes the CPU like the native
  handler and returns the pc to go on at (normally 0x4060f9, after the native handler's
  poll at its `jmp`).
- recomp.py refuses calls to a jump unit, fallthrough into one, and direct jumps into it
  from outside its own body, so the table is the only way in.
- The oracle body of a jump unit ends at the exits (`JUMP_EXITS`, 0x4060f9 and 0x40638d):
  recomp computes it with those as stops, and inside it a jump to an exit goes through
  dispatch. While `x86_stop_esp` is set, dispatch (recomp) or every fetch (interpreter)
  calls `x86_stop_at(pc)`, which stops the run when ESP is above `x86_stop_esp` (a `ret`
  out of the handler) or equal to it at an exit; `x86_stop_pc` is that pc. `game.c` runs
  the oracle with `x86_stop_esp` = the handler's ESP, so the oracle's result is a CPU and
  an exit pc, the same as the C handler's.
- Inside an oracle (`MODE_ORACLE`), `hle_jump` returns `X86_ORACLE_TAG | addr` and the
  dispatch (or the interpreter) runs the original inline. A C handler can do the same
  outside a test: a unit's `native` predicate (Stage 3) leaves the cases it does not cover
  to the original.
- Lockstep: `hle_jump` is an HLE check point like `hle_call`, and an oracle stop is a
  return check point (the shadow predicts the exit pc, not the sentinel).
- Scratch registers: only ESP, EBP and ESI are live at 0x4060f9 (`tools/liveness.py`, an
  interprocedural liveness over the lifted code with per-function summaries), so EAX, ECX,
  EDX, EBX, EDI and the flags are the handlers' scratch (`AT_LOOP` in units.c). Ext 11/12
  return to the caller of the VM, so they have no scratch.

## Differential harness

**Online (`NANPA2_DIFF=1`).** On every call of a unit, `src/game/game.c`:

1. pushes a sentinel return address and saves the CPU (the entry state);
2. runs the lifted original with memory tracing on (`src/game/trace.c`: `rd*`/`wr*` in
   `src/x86/x86.h` report each access while `x86_trace_on` is set; only the tracing
   thread is traced, because the status writer thread reads game memory). The trace keeps
   each touched byte's value before the first access, and whether it was read before being
   written (an input) or written;
3. undoes the original's writes, restores the entry CPU and runs the reimplementation,
   traced;
4. compares: every register outside the unit's scratch set (and ECX's upper half even
   when ECX is scratch, `diff_live_bits`), the flags if a caller uses them, for a jump
   unit the exit pc, every byte either side wrote except stack below the exit ESP (dead
   stack, such as the evaluator's RPN pushes; the stack is the 1 MB below `STACK_TOP`,
   `diff_kept` in diff.h), and every byte the reimplementation read that the original did
   not (an input the record would not hold);
5. on a difference logs `diff MISMATCH addr name call N (esi, frame): ...` and keeps the
   original's result, so the run goes on exactly as the unmodified game. The instruction
   count charged is the original's.

Until stage 2 the output rule was "at or above the entry ESP", and the stack lies below
the game's globals (0x00fxxxxx against 0x4xxxxx), so writes to globals were never
compared: a sabotaged op 0c gave 0 mismatches. With `diff_kept` it gives 3745. Of the stage
1 units only the flag write 0x406455 writes globals; its callers are ops 0a/0b, now in C,
so its writes are compared as theirs (and the unit itself is no longer reached).

**Poison (`NANPA2_DIFF_POISON=1`).** After every unit, compared or not, its scratch
registers get garbage (0xdead0000 ^ ...) and, unless a caller reads them, the flags are
toggled. A register wrongly taken as dead then shows up as a divergence or a trap in the
explore run; this is how the liveness answers (a strong hint, not a proof) are checked.
ECX's upper half is not poisoned (see Stage 3: 04 1f).

Writes the HLE layer makes into game memory with `memcpy` (fread 0x412deb, the GP4
decompressor 0x4075f0, sprintf) report themselves to the trace (`X86_TRACE`), so the
harness also undoes a file the original loaded, for example over the script.

Timer polls are held off during the two runs (`x86_poll_countdown`), so no tick or present
lands between them. Nested units run as originals inside an original and as
reimplementations inside a reimplementation. At exit the run prints per unit: calls,
mismatches, records, instructions per call, and the evaluator's token coverage.

**Records (`NANPA2_DIFF_DIR=DIR`).** Each compared call is also written to
`DIR/<addr>-<pid>.n2diff` (`src/game/diff.c`): entry registers and flags, the input bytes
with their values, exit registers and flags, for a jump unit the exit pc (format
`N2DIFF2`; `N2DIFF1` files of stage 1 are still read), and the output bytes. Records are
deduplicated by a hash of their inputs. Up to `NANPA2_DIFF_MAX` per unit (default 40000) are
kept: every new input for the first half, then one in 16, so a long run is sampled to its
end. Records hold game memory: they stay in the build folder or a scratch folder and are
never committed.

**Replay (`build/diff_replay DIR...`, `tests/diff_replay.c`).** This links only `src/x86/x86.c`
and `src/game` (no SDL, no HLE, no lifted code; `x86_exec` is a stub that traps). For each
record it zeroes memory, writes the inputs, sets the CPU, runs the unit and checks the
outputs. A read of any byte that is not an input counts as a difference, so the replay
also shows that the unit depends on nothing but its inputs. Exit 0 equal, 1 differences, 77
no records.

**ctest.**

- `diffcapture` is a headless explore run (seed 31, 20000 frames, from a new game into the
  Prologue scenes, about 40 s) with `NANPA2_DIFF_DIR=build/diff/ctest`. It fails on
  `MISMATCH`, a trap or a watchdog warning.
- `diffreplay` replays everything in `build/diff` and `build/diff/ctest`, and is skipped
  when there are none. Copy records of longer runs into `build/diff` to have them replayed
  on every `ctest`.

Sabotage check: making operator 0x21 wrap instead of saturating gave 2 mismatching
records out of 19974 in replay.

## Stage 1: script expressions (src/game/expr.c)

Semantics are in `re-notes.md`, "Script VM state" and "Script expressions: exact
semantics". The units:

| Unit | Native | Contract | Scratch |
|---|---|---|---|
| `script_eval` | 0x4037fc (+ operator handlers 0x4038ae-0x4039d1, flag read 0x4039d6) | ESI script offset in/out, EAX value | EBX, EDX, flags (all 12 call sites save or overwrite them) |
| `script_read_constant` | 0x4039f7 (+ 0x403a06, 0x403a12, 0x403a22, 0x403a42) | ESI in/out, EAX value; BX = value for `09` | flags |
| `script_set_flag` | 0x406455 | EBX flag, AX value; AX clamped/shifted as natively | flags |

The C evaluator keeps its own value stack. Inputs the native code cannot survive (a stack
underflow, values left at 0x03, an unknown token after a value) trap with a message in
place of returning into garbage. The one benign case, an unknown token as the first token,
behaves as natively (AL = the token, EAX's upper bytes kept).

Results are in NOTES.md (2026-11-08, reimplementation stage 1).

## Stage 2: VM helpers and the first opcode handlers (src/game/vm.c, ops.c)

Semantics are in `re-notes.md`, "Script VM helpers" and "Script opcodes". Called units
(`HLE_GAME_OVERRIDE`):

| Unit | Native | Contract | Scratch |
|---|---|---|---|
| `vm_skip_block` | 0x4061d4 | ESI in/out, AX = 0 | flags |
| `vm_init` | 0x406395 | no arguments | EAX, flags |
| `vm_menu_item` | 0x4068e1 | ESI in/out, AL / EAX result, EBX text offset | EDX, flags |
| `vm_string_arg` | 0x406dc4 | EDI destination in/out, EBX, AL | flags |
| `vm_parse_args` | 0x40741b (+ 0x4074a3) | ESI in/out, EAX, ECX, EBX, EDI | EDX, flags |

Jump units (`HLE_GAME_JUMP`), all leaving to 0x4060f9 with scratch EAX ECX EDX EBX EDI and
the flags, except ext 11/12, which `ret` and have none: ops 0a 0x406491, 0b 0x4064b5, 0c
0x4064d9, 0d 0x4064f8, 0e 0x406557, 12 0x40663e, 18 0x4069be, and ext 11 0x406638, 12
0x40663b, 22 0x40739b, 23 0x4071a4. The hit test fires the probe 0x4071bb itself (edi = the
rectangle table), so the exploration bot still sees the hotspots. Ext 20 0x4070bc is
written (`game_op_click_handler`) but not enabled: no explore run has reached it yet.

The entry checks for jump units (no calls, fallthrough or outside direct jumps into one)
are made by recomp.py, which the default interp build does not run: build the recomp
backend once after adding an `HLE_GAME_JUMP` or `HLE_GAME_OVERRIDE` line.

The scratch sets come from `tools/liveness.py` and were checked with
`NANPA2_DIFF_POISON=1` explore runs. Results are in NOTES.md (2026-11-08, reimplementation
stage 2).

## Stage 3: the remaining opcode handlers (src/game/ops_sys.c, ops_vm.c)

Every opcode handler of both jump tables is now C except ext 0x20 (0x4070bc, written but
never reached by an explore run). The VM loop 0x4060d4/0x4060f9 itself stays native (see
"Not done" below).

**Handlers around native routines** (`ops_sys.c`, `JUMP_SYS` in units.c): 17 0x406b05,
19 0x407204, 1a 0x407096 and ext 14 0x406842, 15 0x406916, 16 0x406f06, 17 0x406b76, 18
0x406bcb, 19 0x406c20, 1a 0x406c85, 1b 0x406cea, 1c 0x406d8b, 1d 0x406e11, 24 0x4073a5, 25
0x4073cd, 26 0x4073ee. The handler (argument parsing, the sub-op dispatch, the register
set-up) is C; the subsystem routine it calls (blits, file load, the animation step, music)
is still the native one, called with `ncall` as a `call` would. A unit's `native`
predicate sends the sub-ops that are timer-bound or pump messages to the original
handler (19 sub-op 4, the dissolve wipe; 1a sub-op 4, which runs a slot to its end; 04 16
mode 3, a fade that spins on the timer; 04 26 sub-ops 5, 7 and 12; and any sub-op given
as an expression or outside the table): `game_jump` returns
`X86_ORACLE_TAG | addr`, and the diff reports it as "left to the native handler N times".
These units are not recorded (`calls_native`): a replay has no native code to call.

**Call exits** (`ops_vm.c`). Handlers that run a block (0x4061cd), a nested VM (0x4060f9),
a wait or the message pump do it with `call`, and the callee may run any handler, wait
for input or reset the stack. They are split there instead of being nested in C:

- `HLE_GAME_CALL_EXIT(ra, "what")` in overrides.h marks `call target; ra:` inside a jump
  unit. The C handler does what the `call` does (`push32(ra)`, return `target`), so the
  callee runs natively with no host frame under it.
- The code at ra is a jump unit of its own (`RESUME` in units.c), entered when the callee
  returns; `stack_words` is how many words the handler had pushed above its entry ESP.
- recomp.py ends a jump unit's body at its call exits (the call goes through dispatch) and
  checks that each call exit follows a direct call inside a handler.
- The oracle stops (`x86_stop_at`) at an exit pc or any jump unit at the stop ESP (entry
  ESP + 4 * stack_words; not at the first check, which is its own entry), at a call exit's
  target with ra on top of the stack, or when ESP rose above the stop ESP. A
  `mov esp, [mem]` ends its block in the recompile (0x40638d, 0x406fc2), so both backends
  stop right after it.

Units: 0f 0x40659e/0x4065ae/0x4065ed, 11 0x4065f6/0x4065fb, 13 0x406658/0x40666b, 14
0x406a9a/0x406aa7, 15 0x4069ce/0x4069f7 (with the number formatter 0x4069fd, a called
unit), 16 0x406aad, ext 10 0x40660c/0x40661e, ext 13 0x406671 with 22 pieces
0x4066b8-0x40683b, ext 1e 0x406f7c, ext 1f 0x406fcd/0x40704a, ext 21 0x4070d5.

**Harness changes.** An output is dead only below the exit ESP (`diff_kept`): a handler
that ends at a call exit leaves its pushes live. HLE writes into game memory are traced,
so a file load over the script (04 1e, 04 1f) is undone before the C run. ECX's upper half
counts as live even where ECX is scratch (`diff_live_bits`): 04 1f, after the file it
called returns, copies with `mov cx, 0x1e; rep movsd` (0x40705e), and a 04 11/04 12 in an
if block at that file's top level gets there through 0f's `ret` with ECX as the handlers
before it left it. Poisoning all of ECX made the original
copy run off memory.

**Pacing.** The evaluator's 22 per token became per token type (Override mechanism above),
the block skip 4 a byte before the 01 and 6 a byte inside. These were most of the
overcharge in the argument parser (135 against 99 native per call) and ops 0a-0d, 18.

**Not done.**

- The VM loop 0x4060d4/0x4060f9 (fetch, 00/01/02 blocks, text 06 and Big5, the click
  handlers before each fetch). Its handlers are C, but the loop's own work is the pump
  0x401bf0 and the animation step 0x403624 (both native routines that every iteration
  calls) plus the text drawing 0x401900, so a C loop would still call native code around
  each fetch. It is better done with module 5 (text, anim).
- Ext 0x20 0x4070bc: no explore run has reached it.
- `tools/reimpl_status.py` counts a handler's bytes as replaced even where its `native`
  predicate leaves sub-ops to the original (19, 1a, 04 16, 04 26), so its figures are an
  upper bound.

Results are in NOTES.md (2026-11-08, reimplementation stage 3).

## Stage 4: the VM loop, text output and animation (src/game/loop.c, text.c, anim.c)

**The loop as pieces** (`loop.c`). The VM loop is split at its calls as the handlers were in
Stage 3: each piece is a jump unit (`PIECE` in units.c, `HLE_GAME_JUMP` in overrides.h)
and each call that pumps, waits or runs a block is a call exit, so the pump 0x401bf0 runs
once, natively, between two pieces, and a nested block is the same loop on a deeper stack.

| Piece | After | Does |
|---|---|---|
| 0x4060d4 | WinMain's call | VM entry: push ebp, [0x445cdc] = ESP, VM init (0x406395), load the MES file (START.MES), ESI = its entry |
| 0x4060f9 | any handler | loop head: `call 0x401bf0` (call exit 0x4060fe) |
| 0x4060fe | the pump | WM_QUIT: the VM end 0x40638d; else the animation step (C), then unless @[9] bit 2 or a handler is running, test the left button (call exit 0x406124) |
| 0x406124 / 0x406134 | button test 0 / 1 | down with a handler: native (predicate); down without: fetch; up: test the right button / fetch |
| (fetch) | | 0x40616e in C: probe, then 00/02 block end, 01 block, 04 ext table, 06 string, >= 0x80 Big5, 0a-1a op table, the rest the VM end |
| 0x4061c2 / 0x4061cd / 0x4061d3 | a block | skip the block (0x4061d4) and loop; run it (`call 0x4060f9`); return its AL |
| 0x406315 / 0x406383 | a character's delay | advance the cursor (C 0x4063ee), the next string byte / the loop |
| 0x4074d5 / 0x4074db | | input test: pump, test input AL (flags out) |
| 0x4075c6 / 0x4075d7 | | frame wait: pump and step the animations until the frame |
| 0x406acc-0x406b02 | | click wait (five pieces: frame, test, release) |
| 0x406abd / 0x406ac7 | op 16's waits | the op 16 continuations (RESUME), which stage 3 left native |

The text drawing 0x401900, the cursor 0x4063ee/0x406415, the window clear 0x406b24, the
animation step 0x403624 and its slot interpreter 0x403661, the slot control 0x403510 (sub-op
4, which spins on the frame counter, left native), the slot reset 0x406dff and the palette
copy 0x40752c are called units (`CALL_SYS`, `CALL_NATIVE`). The C pieces and handlers call
them directly; a native caller (a handler's sub-op left native) goes through the override. They call the native screen routines (lock
0x401190 and unlock 0x4012b0, blits 0x401430/0x4014f0, fill 0x4017b0, palette upload
0x401ca0) and the GDI imports through `game_ncall`, as the native `call` would (the same
pushes, the same register state), so `src/hle/text.c` and `ddraw.c` serve them unchanged.

**Registers.** At the loop head only ESP, EBP and ESI are live; a piece that ends at a
handler leaves EAX (the opcode index) and EDX (the native map code of 04 26 reads it), a
block end AL and ECX's and EDX's upper halves, so the fetch pieces' scratch is EBX and EDI
(`AT_FETCH`). The text and animation units set every register as the native code does:
their callers' callers read registers in places the liveness pass cannot rule out (the
draw leaves EDX = the scratch surface, which is how the first mismatch of a long run
showed up: a Big5 character with no delay ends at the next piece with EDX live).

**Harness changes.** recomp.py sends a call to a jump unit (the VM entry, 0x4061cd, the
waits) and code that falls into one (0x4060d4 into 0x4060f9) through dispatch, and an
oracle no longer stops at its own entry when that is an exit (the loop head):
`x86_stop_at` never stops at the first check. `x86_is_oracle_pc` takes only a tagged game
code address as an original (the HLE COM thunk 0x71000244 has the tag bit set too). A call
unit's `native` predicate is honoured like a jump unit's. WM_QUIT inside a piece returns
0x40638d at the piece's own ESP, where its oracle stops too.

**Dispatch tables** are held as 16-bit offsets: as 32-bit addresses they are the exe's own
bytes at 0x40620b/0x406262, which ctest `no_game_bytes` found.

**Pacing.** Each piece charges the native instruction count of its path (insns/call of the
oracle and the C agree to 0.1 per piece in the diff report; the VM entry is 102 against
103, once per run).

**Not done.**

- The click handler path 0x40613f-0x40616c (a button down with its handler set): it waits
  for the release and runs the handler's block. Ext 0x20 0x4070bc, which sets the
  handlers, has never been reached by an explore run, so the path is left to the native
  code by the predicates `game_native_click0/1` and the C traps if it ever gets there
  without them.
- 0x403510 sub-op 4 (start a slot and wait until it stops): it spins on the frame counter;
  native, like the other timer-bound sub-ops.
- Not a gap in the C, but a limit of the test: diff runs are slower in game frames than
  plain runs. The timer is real time and the diff runs every piece twice, and since this
  stage that is the whole loop. Seed 31 at NANPA2_TURBO=8 reaches S5.MES at frame 23953
  plain (23258 on stage 3), 33395 with the diff (26758 on stage 3). The exploration bot
  times its clicks in frames, so from a save in OPEN1.MES it stayed there for 300000 frames
  with the diff at turbo 8, and reached 40.MES at turbo 2 (or plain at turbo 8).

Results are in NOTES.md (2026-11-08, reimplementation stage 4).

## Inventory

State: `game` reimplemented in src/game (oracle kept), `hle` replaced by platform code,
`dead` no longer reachable (only called from replaced code). Callers and callees are
function entries (callees outside game code are CRT or MP3 library addresses; Win32 imports
by name).

| Address | Bytes | Module | Purpose | Callers | Callees | State |
|---|---|---|---|---|---|---|
| 0x401000 | 70 | file | fopen "rb": path as given, then with CD prefix 0x431030 | 401050 402cb0 403360 | 412c90 412d13 |  |
| 0x401050 | 258 | file | load whole file (DATA\ or FLAG*), decompress GP4/ANI via 0x4075f0 | 4060d4 406e20 | 401000 4075f0 412c90 412d26 412d6e 412deb 412f02 412f90 ... |  |
| 0x401160 | 47 | file | save(name, buf, size): fopen wb, fwrite, fclose | 4060d4 | 412d13 412d6e 412fb7 |  |
| 0x401190 | 84 | screen | Lock surface (hides cursor on primary), return pointer (+40 lines if 0x4ac068) | 4014f0 401870 401900 401a50 403d45 403e30 ... | 4011f0 |  |
| 0x4011f0 | 184 | screen | cursor: restore saved 32x32 background at 0x4aac54/50 | 401190 401430 4014f0 401670 4017b0 401c60 ... |  |  |
| 0x4012b0 | 41 | screen | Unlock surface; redraw cursor if primary and shown | 4014f0 401870 401900 401a50 403d45 403e30 ... |  |  |
| 0x4012e0 | 330 | screen | cursor: save background, draw AND/XOR bitmap 0x4be900 | 401430 4014f0 401670 4017b0 401c40 |  |  |
| 0x401430 | 178 | screen | Blt copy rect between surfaces 0x44629e[n] (40-line offset) | 403661 4060d4 | 4011f0 4012e0 |  |
| 0x4014f0 | 380 | screen | masked copy rect, index 8 transparent | 403661 4060d4 | 401190 4011f0 4012b0 4012e0 |  |
| 0x401670 | 306 | screen | swap two surface rects through scratch surface 0x4ab058 | 4060d4 | 4011f0 4012e0 |  |
| 0x4017b0 | 186 | screen | colour-fill rect (Blt COLORFILL, colour = arg>>4) | 4060d4 406b24 | 4011f0 4012e0 |  |
| 0x401870 | 130 | screen | XOR rect with colour index (arg>>4) | 4060d4 | 401190 4012b0 |  |
| 0x401900 | 333 | text | draw one ASCII/Big5 character via GDI TextOutA | 4060d4 | 401190 4012b0 SelectObject SetBkColor SetBkMode SetTextColor TextOutA | game |
| 0x401a50 | 402 | screen | dissolve wipe surface 2 -> 0, 2 frames per step | 4060d4 | 401190 4012b0 401bf0 |  |
| 0x401bf0 | 75 | input | message pump (PeekMessage/Dispatch), stream refill 0x4034a0; 0 = WM_QUIT | 401a50 404450 40609c 4060d4 4074d5 4075c6 | 4034a0 DispatchMessageA PeekMessageA TranslateMessage | hle |
| 0x401c40 | 25 | screen | show game cursor (0x4ac510 = 1, draw 0x4012e0) | 4060d4 | 4012e0 |  |
| 0x401c60 | 25 | screen | hide game cursor (restore 0x4011f0, 0x4ac510 = 0) | 4060d4 | 4011f0 |  |
| 0x401c80 | 31 | input | SetCursorPos(x, y+40 if 0x4ac068) | 4060d4 | SetCursorPos |  |
| 0x401ca0 | 138 | screen | build palette from 0x44530c/0x445eae, SetEntries | 403661 4060d4 |  |  |
| 0x401d30 | 221 | screen | start palette fade: per-entry deltas, 0x4ac504 = 1 | 4060d4 |  |  |
| 0x401e10 | 6 | screen | return fade-active flag 0x4ac504 | 4060d4 |  |  |
| 0x401e20 | 431 | startup | play LOGO.AVI through MCI (src/hle/logo.c) | 402090 | 4077a0 412c90 DispatchMessageA GetMessageA SetWindowPos ShowCursor | hle |
| 0x401fd0 | 179 | startup | window procedure during the logo movie |  | DefWindowProcA SetFocus mciSendStringA |  |
| 0x402090 | 292 | startup | WinMain: window, CD search, logo, init, VM, shutdown |  | 401e20 402590 402620 402a20 4060d4 CreateWindowExA GetStockObject LoadCursorA ... |  |
| 0x4021c0 | 699 | input | main window procedure: keys/buttons -> 0x445d8f, mouse, activate |  | 402570 403220 403250 403440 DefWindowProcA DestroyWindow PostQuitMessage SetFocus |  |
| 0x402570 | 25 | screen | WM_PAINT: WaitForVerticalBlank, redraw cursor | 4021c0 | 4011f0 |  |
| 0x402590 | 139 | startup | CD search for SPEECH\A\A10930.WAV, set prefix 0x431030 | 402090 | 412c90 412d13 412d6e |  |
| 0x402620 | 1010 | startup | DirectDraw, surfaces, fonts, DirectSound, 16 ms timer 0x4028c0 | 402090 | 402ad0 4077a0 413140 MessageBoxA PostMessageA ShowCursor timeSetEvent |  |
| 0x402a20 | 173 | startup | shutdown: kill timer, sound exit, release DirectDraw | 402090 | 402c70 ShowCursor timeKillEvent |  |
| 0x402ad0 | 405 | sound | DirectSound init, primary buffer 22050 Hz 8-bit stereo | 402620 | 4077a6 MessageBoxA | hle |
| 0x402c70 | 61 | sound | sound exit: stop voice/music, release DirectSound | 402a20 | 4032c0 403440 | hle |
| 0x402cb0 | 346 | sound | play music n: MUSIC\%02d.MP3 else .WAV | 4060d4 | 401000 402e10 402ea0 402f10 402f60 403020 4030f0 4032c0 ... | hle |
| 0x402e10 | 129 | sound | create MP3 decoder state 0x4be56c | 402cb0 | 407810 407a70 407fb0 408130 40a660 412c90 | dead |
| 0x402ea0 | 112 | sound | set two position notifications on the stream buffer | 402cb0 |  | dead |
| 0x402f10 | 68 | sound | find WAV "data" chunk after fmt | 402cb0 403020 403250 403360 |  | hle |
| 0x402f60 | 187 | sound | Lock sound buffer, copy data, Unlock | 402cb0 403020 4030f0 403250 403360 4034a0 |  | hle |
| 0x403020 | 198 | sound | create static buffer in the WAV's format and fill it | 402cb0 403360 | 402f10 402f60 | dead |
| 0x4030f0 | 215 | sound | create 0x24000-byte MP3 stream buffer, first fill | 402cb0 | 402f60 4031d0 | dead |
| 0x4031d0 | 78 | sound | decode 0x12000 bytes of MP3 (0x409550), rewind at end | 4030f0 4034a0 | 4084b0 409550 | hle |
| 0x403220 | 41 | sound | pause music on deactivate | 4021c0 |  | hle |
| 0x403250 | 111 | sound | resume music after reactivate | 4021c0 | 402f10 402f60 | hle |
| 0x4032c0 | 153 | sound | stop music, free decoder 0x409f40 | 402c70 402cb0 4060d4 | 409f40 412d26 | hle |
| 0x403360 | 211 | sound | play voice SPEECH%s once | 4060d4 | 401000 402f10 402f60 403020 403440 412c90 412d6e 412deb ... | hle |
| 0x403440 | 90 | sound | stop voice buffer 0x4be564 | 4021c0 402c70 403360 | 412d26 | hle |
| 0x4034a0 | 106 | sound | wait on stream events, refill played half | 401bf0 | 402f60 4031d0 MsgWaitForMultipleObjects | hle |
| 0x403510 | 244 | anim | slot control (eax sub-op 0-7: start/run/stop/wait/offset/reset) | 4060d4 406dff | 403661 | game |
| 0x403624 | 61 | anim | once per frame: step the ten slots at 0x4312cc | 4060d4 4075c6 | 403661 | game |
| 0x403661 | 364 | anim | interpret one slot's ANI bytecode (delay, loops, blit, palette) | 403510 403624 | 401430 4014f0 401ca0 40752c | game |
| 0x4037fc | 410 | expr | evaluate RPN expression up to 0x03 | 4060d4 4068e1 40741b | 4039d6 4039f7 | game |
| 0x4039d6 | 33 | expr | read 4-bit flag n from 0x444e98 | 4037fc |  | dead |
| 0x4039f7 | 119 | expr | read constant operand (30-3f, 07 b, 08 h l, 09 h m l) (alt 403a06 403a12 403a22 403a42) | 4037fc 4060d4 |  | game |
| 0x403a70 | 23 | map | ext op 0x26 dispatcher: args to 0x43555e, call 0x435576[op] | 4060d4 |  |  |
| 0x403a87 | 98 | map | sub-op 0: init map, walk grid base 0x435574, reset events |  |  |  |
| 0x403ae9 | 63 | map | sub-op 1: ? writes 0xc1/0xc3 into sprite script 0 |  |  |  |
| 0x403b28 | 170 | map | sub-op 2: fill view cell grid 0x4313c8 from tile layer | 4042e7 4051c2 |  |  |
| 0x403bd2 | 371 | map | sub-op 3: overlay visible sprites' tiles into the cell grid | 4042e7 4051c2 |  |  |
| 0x403d45 | 235 | map | sub-op 4: redraw cells changed since 0x433308 | 4042e7 4051c2 | 401190 4012b0 403eff |  |
| 0x403e30 | 207 | map | sub-op 5: redraw every cell, then event check 0x405216 |  | 401190 4012b0 403eff 4040f0 405216 |  |
| 0x403eff | 69 | map | draw one 16x16 cell: tile via 0x4355fa, 0x435dfc[layers] | 403d45 403e30 |  |  |
| 0x403f44 | 107 | map | cell draw [0x435dfc 0]: tile only |  |  |  |
| 0x403faf | 60 | map | cell draw [0x435dfc 1]: tile + 1 sprite layer |  | 404132 4041f9 404281 |  |
| 0x403feb | 107 | map | cell draw [0x435dfc 2]: tile + 2 sprite layers |  | 404132 4041f9 404281 |  |
| 0x404056 | 154 | map | cell draw [0x435dfc 3]: tile + 3 sprite layers |  | 404132 4041f9 404281 |  |
| 0x4040f0 | 66 | map | clear bit 3 of a 16x16 screen cell | 403e30 |  |  |
| 0x404132 | 199 | map | load tile into work cell 0x435248 plus mask planes | 403faf 403feb 404056 |  |  |
| 0x4041f9 | 136 | map | overlay one sprite tile onto the work cell | 403faf 403feb 404056 |  |  |
| 0x404281 | 102 | map | write the composed work cell to the screen | 403faf 403feb 404056 |  |  |
| 0x4042e7 | 16 | map | sub-op 6: compose and draw (sub-ops 2, 3, 4) |  | 403b28 403bd2 403d45 |  |
| 0x4042f7 | 36 | map | sub-op 7: step all sprite scripts | 4051c2 | 40431b |  |
| 0x40431b | 245 | map | step one sprite script (move kind/count) | 4042f7 | 404410 |  |
| 0x404410 | 30 | map | count down move steps, dispatch 0x435e0c[kind-1] | 40431b |  |  |
| 0x40442e | 15 | map | move kind 1: up (0x404731), toggle frame |  | 404731 |  |
| 0x404435 | 7 | map | move kind 2: down (0x404937) |  | 404937 |  |
| 0x40443c | 7 | map | move kind 3: left (0x404a9a) |  | 404a9a |  |
| 0x404443 | 5 | map | move kind 4: right (0x404c9f) |  | 404c9f |  |
| 0x404450 | 615 | map | move kind 6: player control, mouse/keys, history ring 0x435dca |  | 401bf0 404731 404937 404a9a 404c9f 405597 405691 40609c |  |
| 0x4046b7 | 15 | map | move kind 8: left+up |  | 404731 404a9a |  |
| 0x4046c6 | 15 | map | move kind 9: right+up |  | 404731 404c9f |  |
| 0x4046d5 | 15 | map | move kind 10: left+down |  | 404937 404a9a |  |
| 0x4046e4 | 15 | map | move kind 11: right+down |  | 404937 404c9f |  |
| 0x4046f3 | 54 | map | move kind 13: walk to target (0x405966 / 0x405691) |  | 405691 405966 |  |
| 0x404731 | 727 | map | move sprite up: walls, sliding, camera scroll | 40442e 404450 4046b7 4046c6 405691 | 404e06 404e25 |  |
| 0x404937 | 355 | map | move sprite down | 404435 404450 4046d5 4046e4 405691 | 404e06 404e25 |  |
| 0x404a9a | 428 | map | move sprite left | 40443c 404450 4046b7 4046d5 405691 | 404e06 404e25 |  |
| 0x404c9f | 239 | map | move sprite right | 404443 404450 4046c6 4046e4 405691 | 404e06 404e25 |  |
| 0x404e06 | 31 | map | map cell address from (bx = x, cx = y) | 404731 404937 404a9a 404c9f |  |  |
| 0x404e25 | 359 | map | sprite-sprite collision ahead; ax = sprite index or -1 | 404731 404937 404a9a 404c9f 40500e |  |  |
| 0x404f8c | 130 | map | sub-op 8: area trigger under sprite n | 405216 |  |  |
| 0x40500e | 30 | map | sub-op 9: collision test for sprite n | 405216 | 404e25 |  |
| 0x40502c | 231 | map | sub-op 10: place player/view at entry n, fill history |  |  |  |
| 0x405113 | 175 | map | sub-op 11: reset history to player, view from entry n |  |  |  |
| 0x4051c2 | 84 | map | sub-op 12: one map frame (falls into 0x405216) |  | 403b28 403bd2 403d45 4042f7 40609c |  |
| 0x405216 | 400 | map | event check: sprite contact / area trigger | 403e30 | 404f8c 40500e 40609c |  |
| 0x4053a6 | 56 | map | sub-op 13: set sprite n's script |  |  |  |
| 0x4053de | 55 | map | sub-op 14: enable/disable sprite n (flag 0x80) |  |  |  |
| 0x405415 | 122 | map | sub-op 15: unpack run-length layer (0x4000 = run) |  |  |  |
| 0x40548f | 179 | map | sub-op 16: copy a rectangle of map cells |  |  |  |
| 0x405542 | 77 | map | sub-op 30: start pathfinding for sprite n |  | 405966 |  |
| 0x40558f | 8 | map | sub-op 32: cancel auto-walk (0x435573 = 0) |  |  |  |
| 0x405597 | 148 | map | right click: mouse to map cell, pathfind the player there | 404450 | 405966 40609c |  |
| 0x40562b | 102 | map | build walk grid from cell bit 0x80 (0xff wall, 0xfe free) | 405966 |  |  |
| 0x405691 | 725 | map | auto-walk step: go to the lower neighbour (8 directions) | 404450 4046f3 | 404731 404937 404a9a 404c9f |  |
| 0x405966 | 359 | map | pathfinding setup: start/goal, walk grid, flood fill | 4046f3 405542 405597 | 40562b 405acd 405fb6 |  |
| 0x405acd | 58 | map | start the recursive flood fill 0x405b07 from the goal | 405966 | 405b07 |  |
| 0x405b07 | 397 | map | recursive flood fill with direction-order mask | 405acd 405c72 405ce8 405d3a 405d8c 405e05 ... |  |  |
| 0x405c72 | 137 | map | flood-fill order case 0 (0x435e40[0]) |  | 405b07 |  |
| 0x405ce8 | 118 | map | flood-fill order case 1 |  | 405b07 |  |
| 0x405d3a | 82 | map | flood-fill order case 2 |  | 405b07 |  |
| 0x405d8c | 77 | map | flood-fill order case 3 |  | 405b07 |  |
| 0x405e05 | 123 | map | flood-fill order case 4 |  | 405b07 |  |
| 0x405e5a | 83 | map | flood-fill order case 5 |  | 405b07 |  |
| 0x405ed0 | 109 | map | flood-fill order case 6 |  | 405b07 |  |
| 0x405f24 | 73 | map | flood-fill order case 7 |  | 405b07 |  |
| 0x405fb6 | 230 | map | unreachable goal: mark approach cells for the sprite footprint | 405966 |  |  |
| 0x40609c | 64 | input | pump (WM_QUIT resets esp 0x445cdc); bl = buttons, cx/dx = mouse | 404450 4051c2 405216 405597 | 401bf0 |  |
| 0x4060d4 | 3765 | vm | VM: load start.mes, fetch loop 0x4060f9, all opcode handlers (alt 4060f9) | 402090 4060d4 4061cd | 401050 401160 401190 4012b0 401430 4014f0 401670 4017b0 ... | game |
| 0x4061cd | 7 | vm | run a nested block (skip 01, recursive 0x4060f9) | 4060d4 | 4060d4 | game |
| 0x4061d4 | 39 | vm | skip a 01 ... 00 block, counting nesting | 4060d4 40741b |  | game |
| 0x406395 | 89 | vm | init register '@' and segment table 0x44533c | 4060d4 |  | game |
| 0x4063ee | 103 | text | advance text cursor, wrap line/window (alt 0x406415 newline) (alt 406415) | 4060d4 |  | game |
| 0x406455 | 60 | expr | write 4-bit flag n = ax (clamped to 15) | 4060d4 |  | game |
| 0x4068e1 | 53 | vm | menu list: next item (guard 0f expr), al = -1/0/1 | 4060d4 | 4037fc | game |
| 0x4069fd | 157 | text | format a number as ASCII or Big5 digits into 0x445ce6 (alt 406a66) | 4060d4 4069fd | 4069fd |  |
| 0x406acc | 57 | vm | wait for click/Enter (bit 0x10) press then release | 4060d4 | 4074d5 4075c6 | game |
| 0x406b24 | 82 | text | clear text window (fill 0x4017b0), home cursor | 4060d4 | 4017b0 | game |
| 0x406dc4 | 59 | vm | copy string argument into buffer edi | 4060d4 406e20 |  | game |
| 0x406dff | 18 | anim | reset all animation slots (0x403510 sub-op 7) | 4060d4 406e20 | 403510 | game |
| 0x406e20 | 230 | screen | load GP4 to 0x4662be, set palette, copy to surface @[6] | 4060d4 | 401050 401190 4012b0 406dc4 406dff 40752c |  |
| 0x40741b | 136 | vm | parse argument list into 0x445d06 (types 0x445cf6), eax = arg 0 | 4060d4 | 4037fc 4061d4 4074a3 | game |
| 0x4074a3 | 50 | vm | test whether the next byte starts an expression | 40741b |  | dead |
| 0x4074d5 | 87 | input | pump, then test input bit al (0 ok, 1 cancel, 2-5 arrows) | 4060d4 406acc | 401bf0 | game |
| 0x40752c | 33 | screen | copy a picture palette into 0x44530c / 0x445eae | 403661 406e20 |  | game |
| 0x40754d | 31 | screen | fill palette tables with one value (fade target) | 4060d4 |  |  |
| 0x40756c | 45 | screen | save palette tables to 0x44608e | 4060d4 |  |  |
| 0x407599 | 45 | screen | restore palette tables from 0x44608e | 4060d4 |  |  |
| 0x4075c6 | 40 | vm | wait ecx/4+1 frames, pump and animate | 4060d4 406acc | 401bf0 403624 | game |
| 0x4075f0 | 140 | file | GP4/ANI LZHUF decompressor (src/lzh.c) | 401050 | 40776b | hle |
| 0x40767c | 61 | file | LZHUF tree rebuild at frequency 0x8000 | 4076b9 | 4076b9 | dead |
| 0x4076b9 | 178 | file | LZHUF update (alt 0x4076cb) (alt 4076cb) | 40767c 40776b | 40767c | dead |
| 0x40776b | 52 | file | LZHUF decode one symbol, update | 4075f0 | 4076b9 | dead |
| 0x4077a0 | 6 | startup | thunk jmp [0x41b000] (DirectDrawCreate) | 401e20 402620 |  |  |
| 0x4077a6 | 6 | startup | thunk jmp [0x41b008] (DirectSoundCreate) | 402ad0 |  | dead |

## Script opcodes (units for the jump-override stage)

Fetched at 0x40616e. These are handled inline: 00/02 end a block (0x4061aa, which returns
from a nested VM), 01 runs a nested block (0x4061bc), 04 xx is an extended opcode, 06 is
ASCII text up to 06 (0x4062be), bytes from 0x80 are one Big5 character (0x40631c; A1BE is a
newline, 0x406415), and 03/05/07/08 end the VM (0x40638d). Before each fetch, unless bit 2
of @[9] (0x4452e0) is set, the click handlers set by ext 0x20 (0x445ce2/ce4) run. Op 09
and 0x1b-0x1f pass the range check but index outside table 0x40620b, so they are undefined.

| Op | Handler | Purpose |
|---|---|---|
| 0a | 0x406491 | set flag n (constant) = expr [, next flag = expr ...] |
| 0b | 0x4064b5 | set flag (index an expression) = expr [, ...] |
| 0c | 0x4064d9 | set register R = expr |
| 0d | 0x4064f8 | store words: seg[@[7]][R + 2*i] = expr, ... (R = `@`: segment 0) |
| 0e | 0x406557 | store bytes: seg[@[7]][R + i] = expr, ... |
| 0f | 0x40659e | if expr block, else-if chain (02 0f ...) |
| 10 | 0x4060f9 | no-op |
| 11 | 0x4065f6 | wait click (0x406acc), clear text window |
| 12 | 0x40663e | set label n = offset (table 0x445dbc) |
| 13 | 0x406658 | call label n (nested VM) |
| 14 | 0x406a9a | call script offset (nested VM) |
| 15 | 0x4069ce | print number (format @ 0x4452f7) |
| 16 | 0x406aad | wait n frames (0x4075c6); no argument: wait click |
| 17 | 0x406b05 | clear text window (optional colour) |
| 18 | 0x4069be | set text colour 0x4452f4 |
| 19 | 0x407204 | misc sub-ops (table 0x407216; restore stash, fade/stopwatch 0x4072b7, row swap, wipe 0x401a50), result in S |
| 1a | 0x407096 | animation slot control (0x403510) |
| 04 10 | 0x40660c | while expr block |
| 04 11 | 0x406638 | continue (block returns al = 1) |
| 04 12 | 0x40663b | break (block returns al = 2) |
| 04 13 | 0x406671 | menu/event loop: labels 38 init, 39 per frame, 32-37 input |
| 04 14 | 0x406842 | collect visible menu items into 0x4452fa |
| 04 15 | 0x406916 | cursor sub-ops (table 0x406922) |
| 04 16 | 0x406f06 | palette: set now / fade and wait |
| 04 17 | 0x406b76 | fill rect (0x4017b0) on surface @[6] |
| 04 18 | 0x406bcb | XOR rect (0x401870) |
| 04 19 | 0x406c20 | copy rect between surfaces (0x401430) |
| 04 1a | 0x406c85 | swap rect between surfaces (0x401670) |
| 04 1b | 0x406cea | masked copy rect (0x4014f0) |
| 04 1c | 0x406d8b | load file into segment @[7] at offset |
| 04 1d | 0x406e11 | load GP4 and draw to surface @[6] (0x406e20) |
| 04 1e | 0x406f7c | jump to MES file |
| 04 1f | 0x406fcd | call MES file as subroutine |
| 04 20 | 0x4070bc | set click handler label (0 left, 1 right) |
| 04 21 | 0x4070d5 | FLAGn: 0 load and restart, 1 save 0xc00 bytes, 2 load keeping 32 bytes |
| 04 22 | 0x40739b | parse arguments, no-op |
| 04 23 | 0x4071a4 | hotspot hit test -> S |
| 04 24 | 0x4073a5 | music: first argument 0 plays MUSIC n (second argument), else stops |
| 04 25 | 0x4073cd | play voice clip |
| 04 26 | 0x4073ee | map/sprite sub-op via 0x403a70 -> S |
