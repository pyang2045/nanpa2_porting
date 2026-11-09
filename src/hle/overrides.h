/*
 * The one list of native game functions replaced by hand-written code, and
 * of the timing probes. Both backends read it: the interpreter
 * (src/x86/interp.c) includes it, and tools/recomp.py parses the
 * HLE_OVERRIDE / HLE_GAME_OVERRIDE / HLE_PROBE lines (one entry per line, hex address
 * first). Calls to and dispatches into an override address go to
 * hle_call(), which runs the replacement (src/hle/hle.c override_call or a
 * module it calls).
 *
 * HLE_GAME_OVERRIDE: game logic reimplemented in C (src/game,
 * docs/reimpl.md). An override like the others, but its original body is
 * kept as the differential-test oracle: x86_exec(X86_ORACLE_TAG | addr)
 * runs it, skipping the override for that entry (recomp: the lifted body;
 * interpreter: the decoded body, where direct jumps back to the entry from
 * inside it stay in the original as well). recomp.py reads these lines into
 * GAME_OVERRIDES.
 *
 * HLE_GAME_JUMP: a script opcode handler reimplemented in C (src/game/ops.c).
 * The VM loop enters a handler by an indirect jmp and the handler leaves by
 * jmp to the loop head 0x4060f9 (or the VM end 0x40638d, or ret), so there
 * is no return address: reaching addr by any jump continues at
 * pc = hle_jump(addr), the pc the C handler returns. Never called and never
 * fallen into (recomp.py checks). The original body is kept as the oracle
 * like HLE_GAME_OVERRIDE's; it is run until it reaches an exit with the
 * handler's ESP (x86_stop_esp, src/x86/x86.h). recomp.py reads these lines
 * into GAME_JUMP_OVERRIDES.
 *
 * HLE_GAME_CALL_EXIT(ra, what): a call made by an opcode handler that the
 * C handler does not run itself: it pushes ra and continues at the callee,
 * as the native `call` does, so blocks, waits and the message pump run
 * once, natively, with no host frames under them. ra is the return
 * address; the code there is usually a jump unit of its own (the rest of
 * the handler). The handler's oracle ends at the call (pc = the callee
 * with ra on top of the stack); recomp.py routes such calls through
 * dispatch, where the test can stop them, and reads these lines into
 * CALL_EXITS.
 *
 * HLE_GAME_DEAD(addr, what): native code whose work is in C and that no
 * running code enters any more: it is reached only from replaced code (the
 * map's cell drawing and flood fill, which are C functions below the units)
 * or only through the cases a unit's `native` predicate leaves to the
 * original (the map sub-ops a script runs are C; the pumping ones are
 * not). Nothing hooks it: the lifted code stays, as the oracle's body.
 * Only tools/reimpl_status.py reads these lines, to count it as replaced.
 *
 * Use as an X-macro: define HLE_OVERRIDE(addr, what), HLE_GAME_OVERRIDE,
 * HLE_GAME_JUMP, HLE_GAME_CALL_EXIT and/or HLE_PROBE(addr, what), then
 * include this file. No include guard.
 */
#ifndef HLE_OVERRIDE
#define HLE_OVERRIDE(addr, what)
#define HLE_OVERRIDES_UNDEF_O
#endif
#ifndef HLE_GAME_OVERRIDE
#define HLE_GAME_OVERRIDE(addr, what)
#define HLE_OVERRIDES_UNDEF_G
#endif
#ifndef HLE_GAME_JUMP
#define HLE_GAME_JUMP(addr, what)
#define HLE_OVERRIDES_UNDEF_J
#endif
#ifndef HLE_GAME_CALL_EXIT
#define HLE_GAME_CALL_EXIT(ra, what)
#define HLE_OVERRIDES_UNDEF_C
#endif
#ifndef HLE_GAME_DEAD
#define HLE_GAME_DEAD(addr, what)
#define HLE_OVERRIDES_UNDEF_D
#endif
#ifndef HLE_PROBE
#define HLE_PROBE(addr, what)
#define HLE_OVERRIDES_UNDEF_P
#endif

HLE_OVERRIDE(0x401e20, "logo movie (MCI avivideo)")
HLE_OVERRIDE(0x401bf0, "message pump")
HLE_OVERRIDE(0x4028c0, "multimedia timer callback (x87 palette fade)")
HLE_OVERRIDE(0x402ad0, "DirectSound init")
HLE_OVERRIDE(0x402c70, "sound shutdown")
HLE_OVERRIDE(0x402f10, "find WAV data chunk")
HLE_OVERRIDE(0x402f60, "DirectSound buffer write")
HLE_OVERRIDE(0x4031d0, "MP3 decode into buffer")
HLE_OVERRIDE(0x403220, "music pause")
HLE_OVERRIDE(0x403250, "music resume")
HLE_OVERRIDE(0x4032c0, "music stop")
HLE_OVERRIDE(0x403440, "voice stop")
HLE_OVERRIDE(0x4034a0, "music stream pump")
HLE_OVERRIDE(0x4075f0, "GP4/ANI decompressor (src/lzh.c)")
HLE_OVERRIDE(0x402cb0, "play music N (MUSIC\\%02d.MP3 through the linked MP3 decoder)")
HLE_OVERRIDE(0x403360, "play voice (SPEECH%s through DirectSound)")

/* Reimplemented game logic (src/game/expr.c; game_units in src/game/units.c
 * holds the C side) */

/* Script opcode handlers in C (src/game/ops.c) */
/* 0x4070bc (op 04 20, click handler): used by no shipped script; checked with
 * the dev hook NANPA2_CLICKTEST and the fuzz (src/game/map_fuzz.c) */

/* Handlers around native subsystem routines (src/game/ops_sys.c); 19, 1a,
 * 04 16 and 04 26 leave their timer-bound sub-ops to the original */
HLE_GAME_JUMP(0x406842, "op 04 14: menu item table")
HLE_GAME_JUMP(0x406916, "op 04 15: cursor")
HLE_GAME_JUMP(0x406b05, "op 17: clear text window")
HLE_GAME_JUMP(0x406b76, "op 04 17: fill rectangle")
HLE_GAME_JUMP(0x406bcb, "op 04 18: XOR rectangle")
HLE_GAME_JUMP(0x406c20, "op 04 19: copy rectangle")
HLE_GAME_JUMP(0x406c85, "op 04 1a: swap rectangle")
HLE_GAME_JUMP(0x406cea, "op 04 1b: masked copy rectangle")
HLE_GAME_JUMP(0x406d8b, "op 04 1c: load file into a segment")
HLE_GAME_JUMP(0x406e11, "op 04 1d: picture")
HLE_GAME_JUMP(0x406f06, "op 04 16: palette")
HLE_GAME_JUMP(0x407096, "op 1a: animation slot")
HLE_GAME_JUMP(0x407204, "op 19: misc sub-ops")
HLE_GAME_JUMP(0x4073a5, "op 04 24: music")
HLE_GAME_JUMP(0x4073cd, "op 04 25: voice")
HLE_GAME_JUMP(0x4073ee, "op 04 26: map")

/* Handlers that run a block, a script or a wait, split at those calls
 * (src/game/ops_vm.c): each call is a call exit, the code after it a
 * jump unit */

/* Stage 4: the VM loop and the waits (src/game/loop.c), split at the calls
 * that pump messages, wait or run a block like the handlers above. They
 * are entered by call too (0x4061cd, 0x4074d5,
 * 0x4075c6 and 0x406acc from the handlers): recomp.py sends such calls
 * through dispatch. The click handler path after 0x406124/0x406134
 * (0x40613f-0x40616c) stays native with ext 20. */

/* The town map (src/game/map*.c): the 04 26 handler 0x4073ee runs the whole
 * map in direct-style C (map_pump.c, through ctl_piece); nothing else enters
 * the native map code (0x403a70-0x4060a2), which is dead below. */
HLE_GAME_DEAD(0x403b28, "map: sub-op 2, the cell grid from the layer (src/game/map_draw.c)")
HLE_GAME_DEAD(0x403bd2, "map: sub-op 3, the sprite tiles over the grid (src/game/map_draw.c)")
HLE_GAME_DEAD(0x403d45, "map: sub-op 4, draw the changed cells (src/game/map_draw.c)")
HLE_GAME_DEAD(0x403eff, "map: draw one cell (src/game/map_draw.c)")
HLE_GAME_DEAD(0x4040f0, "map: clear bit 3 of a cell (src/game/map_draw.c)")
HLE_GAME_DEAD(0x404731, "map: move a sprite up (src/game/map_move.c)")
HLE_GAME_DEAD(0x404937, "map: move a sprite down (src/game/map_move.c)")
HLE_GAME_DEAD(0x404a9a, "map: move a sprite left (src/game/map_move.c)")
HLE_GAME_DEAD(0x404c9f, "map: move a sprite right (src/game/map_move.c)")
HLE_GAME_DEAD(0x405691, "map: one step of an auto-walk (src/game/map_path.c)")
HLE_GAME_DEAD(0x405966, "map: plan a walk to a cell (src/game/map_path.c)")
HLE_GAME_DEAD(0x404f8c, "map: area trigger under a sprite (src/game/map.c)")
HLE_GAME_DEAD(0x40500e, "map: collision test for a sprite (src/game/map.c)")

HLE_GAME_DEAD(0x40609c, "map pump: pushad, message pump")
HLE_GAME_DEAD(0x4060a2, "map pump: after the message pump, popad, buttons and mouse")
HLE_GAME_DEAD(0x40448b, "player: after the pump in the cursor path")
HLE_GAME_DEAD(0x4044a9, "player: after the pump for the left button")
HLE_GAME_DEAD(0x4044b7, "player: after the pump, follow the mouse")
HLE_GAME_DEAD(0x4045eb, "player: after the pump in the keyboard path")
HLE_GAME_DEAD(0x4051e3, "map frame: after the pump, a click on the screen's button")
HLE_GAME_DEAD(0x40520f, "map frame: after the pump, wait for the button to go up")
HLE_GAME_DEAD(0x405233, "event check: after the first pump")
HLE_GAME_DEAD(0x405273, "event check: after the second pump")
HLE_GAME_DEAD(0x40539f, "event check: after the pump, wait for the button to go up")
HLE_GAME_DEAD(0x40559d, "right click: after the pump, plan the walk")
HLE_GAME_DEAD(0x407410, "op 04 26: after the map sub-op and the pop of ESI")
/* the native bodies are the oracles' code and nothing else enters them */
HLE_GAME_DEAD(0x403a70, "map: 04 26 dispatcher")
HLE_GAME_DEAD(0x403e30, "map: sub-op 5")
HLE_GAME_DEAD(0x4042f7, "map: sub-op 7")
HLE_GAME_DEAD(0x40431b, "map: one sprite's script step")
HLE_GAME_DEAD(0x404410, "map: sprite movement kind dispatch")
HLE_GAME_DEAD(0x404448, "map: kind 5 (stand, flip the frame; the tail of the kind stubs)")
HLE_GAME_DEAD(0x40444f, "map: kind 7 (stand)")
HLE_GAME_DEAD(0x404450, "map: kind 6, the player")
HLE_GAME_DEAD(0x4051c2, "map: sub-op 12")
HLE_GAME_DEAD(0x405216, "map: event check")
HLE_GAME_DEAD(0x405597, "map: right click")

/* Stage 5A: native map code whose work is in C and that nothing enters now
 * (see HLE_GAME_DEAD above) */
HLE_GAME_DEAD(0x403a87, "map: sub-op 0 (init)")
HLE_GAME_DEAD(0x403ae9, "map: sub-op 1")
HLE_GAME_DEAD(0x403f44, "map: cell draw, no sprite tile")
HLE_GAME_DEAD(0x403faf, "map: cell draw, 1 sprite tile")
HLE_GAME_DEAD(0x403feb, "map: cell draw, 2 sprite tiles")
HLE_GAME_DEAD(0x404056, "map: cell draw, 3 sprite tiles")
HLE_GAME_DEAD(0x404132, "map: tile into the work planes")
HLE_GAME_DEAD(0x4041f9, "map: sprite tile over the work cell")
HLE_GAME_DEAD(0x404281, "map: work cell to the screen")
HLE_GAME_DEAD(0x4042e7, "map: sub-op 6")
HLE_GAME_DEAD(0x40442e, "map: kind 1")
HLE_GAME_DEAD(0x404435, "map: kind 2")
HLE_GAME_DEAD(0x40443c, "map: kind 3")
HLE_GAME_DEAD(0x404443, "map: kind 4")
HLE_GAME_DEAD(0x4046b7, "map: kind 8")
HLE_GAME_DEAD(0x4046c6, "map: kind 9")
HLE_GAME_DEAD(0x4046d5, "map: kind 10")
HLE_GAME_DEAD(0x4046e4, "map: kind 11")
HLE_GAME_DEAD(0x4046f3, "map: kind 13")
HLE_GAME_DEAD(0x404e06, "map: tile word address")
HLE_GAME_DEAD(0x404e25, "map: sprite collision test")
HLE_GAME_DEAD(0x40502c, "map: sub-op 10")
HLE_GAME_DEAD(0x405113, "map: sub-op 11")
HLE_GAME_DEAD(0x4053a6, "map: sub-op 13")
HLE_GAME_DEAD(0x4053de, "map: sub-op 14")
HLE_GAME_DEAD(0x405415, "map: sub-op 15")
HLE_GAME_DEAD(0x40548f, "map: sub-op 16")
HLE_GAME_DEAD(0x405542, "map: sub-op 30")
HLE_GAME_DEAD(0x40558f, "map: sub-op 32")
HLE_GAME_DEAD(0x40562b, "map: walk grid")
HLE_GAME_DEAD(0x405acd, "map: start the flood fill")
HLE_GAME_DEAD(0x405b07, "map: flood fill")
HLE_GAME_DEAD(0x405c72, "map: flood case 0")
HLE_GAME_DEAD(0x405ce8, "map: flood case 1")
HLE_GAME_DEAD(0x405d3a, "map: flood case 2")
HLE_GAME_DEAD(0x405d8c, "map: flood case 3")
HLE_GAME_DEAD(0x405e05, "map: flood case 4")
HLE_GAME_DEAD(0x405e5a, "map: flood case 5")
HLE_GAME_DEAD(0x405ed0, "map: flood case 6")
HLE_GAME_DEAD(0x405f24, "map: flood case 7")
HLE_GAME_DEAD(0x405fb6, "map: approach cells")

/* op 1a (src/game/ops_sys.c) runs the slot control (anim.c) directly and
 * goes on at 0x4070b6 */
HLE_GAME_JUMP(0x4070b6, "op 1a: after the slot control (src/game/ops_sys.c)")
/* 04 16 mode 3 waits for the fade in 0x406f41 (src/game/screen.c, ops_sys.c) */

/* Stage 5B (src/game/screen.c): the surface lock and unlock and the cursor
 * restore, which native code (the map, the window procedure) still calls.
 * The other screen routines (cursor draw, show, hide, blits, fill, XOR,
 * palette, fades) are called only from C and have no override. */
HLE_GAME_OVERRIDE(0x401190, "surface lock (src/game/screen.c)")
HLE_GAME_OVERRIDE(0x4012b0, "surface unlock (src/game/screen.c)")

/* the logo's window procedure: installed (SetWindowLongA) only by the logo
 * player 0x401e20, an HLE override, so nothing enters it */
HLE_GAME_DEAD(0x401fd0, "logo window procedure (logo player is HLE)")

/* Stage 6: the dissolve wipe (src/game/wipe.c): op 19 sub-op 4 calls it as a
 * call exit (return 0x40738b), it pumps messages in between (0x401b90) */
HLE_GAME_JUMP(0x40738b, "op 19 4: after the wipe")
HLE_GAME_CALL_EXIT(0x40738b, "op 19 4: the dissolve wipe (0x401a50)")

/* Timing probes (x86_probe, logged with NANPA2_PROBE=1), run before these
 * instructions: wipe 0x401a50 entry and its return in opcode 0x407386, the
 * two text-character draws (0x4062ff ASCII, 0x40636d Big5), and the
 * wait-for-click at 0x406acc; 0x40616e is the script VM's opcode fetch
 * (logged with NANPA2_VMTRACE=1, watched by the exploration bot); 0x4071bb
 * is inside the script hit test 0x4071a4 with edi = its rectangle table
 * (NANPA2_HOTSPOTS=1; its C handler in src/game/ops.c fires it too). */
HLE_PROBE(0x40616e, "script VM opcode fetch")
HLE_PROBE(0x40738b, "wipe return in opcode 0x407386")
HLE_PROBE(0x4062ff, "ASCII character draw")
HLE_PROBE(0x40636d, "Big5 character draw")
HLE_PROBE(0x406acc, "wait for click")
HLE_PROBE(0x4071bb, "script hit test rectangle table")

#ifdef HLE_OVERRIDES_UNDEF_O
#undef HLE_OVERRIDE
#undef HLE_OVERRIDES_UNDEF_O
#endif
#ifdef HLE_OVERRIDES_UNDEF_G
#undef HLE_GAME_OVERRIDE
#undef HLE_OVERRIDES_UNDEF_G
#endif
#ifdef HLE_OVERRIDES_UNDEF_J
#undef HLE_GAME_JUMP
#undef HLE_OVERRIDES_UNDEF_J
#endif
#ifdef HLE_OVERRIDES_UNDEF_C
#undef HLE_GAME_CALL_EXIT
#undef HLE_OVERRIDES_UNDEF_C
#endif
#ifdef HLE_OVERRIDES_UNDEF_D
#undef HLE_GAME_DEAD
#undef HLE_OVERRIDES_UNDEF_D
#endif
#ifdef HLE_OVERRIDES_UNDEF_P
#undef HLE_PROBE
#undef HLE_OVERRIDES_UNDEF_P
#endif
