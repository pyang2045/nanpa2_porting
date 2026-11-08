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
 * Use as an X-macro: define HLE_OVERRIDE(addr, what), HLE_GAME_OVERRIDE,
 * HLE_GAME_JUMP and/or HLE_PROBE(addr, what), then include this file. No
 * include guard.
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
HLE_GAME_OVERRIDE(0x4037fc, "script expression evaluator (src/game/expr.c)")
HLE_GAME_OVERRIDE(0x4039f7, "script constant reader (src/game/expr.c)")
HLE_GAME_OVERRIDE(0x406455, "script flag write (src/game/expr.c)")
HLE_GAME_OVERRIDE(0x4061d4, "VM skip block (src/game/vm.c)")
HLE_GAME_OVERRIDE(0x406395, "VM init (src/game/vm.c)")
HLE_GAME_OVERRIDE(0x4068e1, "VM menu item (src/game/vm.c)")
HLE_GAME_OVERRIDE(0x406dc4, "VM string argument (src/game/vm.c)")
HLE_GAME_OVERRIDE(0x40741b, "VM argument list parser (src/game/vm.c)")

/* Script opcode handlers in C (src/game/ops.c) */
HLE_GAME_JUMP(0x406491, "op 0a: set flags")
HLE_GAME_JUMP(0x4064b5, "op 0b: set flags from expressions")
HLE_GAME_JUMP(0x4064d9, "op 0c: set register")
HLE_GAME_JUMP(0x4064f8, "op 0d: store words")
HLE_GAME_JUMP(0x406557, "op 0e: store bytes")
HLE_GAME_JUMP(0x406638, "op 04 11: continue")
HLE_GAME_JUMP(0x40663b, "op 04 12: break")
HLE_GAME_JUMP(0x40663e, "op 12: set label")
HLE_GAME_JUMP(0x4069be, "op 18: text colour")
/* 0x4070bc (op 04 20, click handler): game_op_click_handler in ops.c is
 * written but not enabled until an explore run reaches it under the diff. */
HLE_GAME_JUMP(0x4071a4, "op 04 23: hit test")
HLE_GAME_JUMP(0x40739b, "op 04 22: arguments only")

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

/* Timing probes (x86_probe, logged with NANPA2_PROBE=1), run before these
 * instructions: wipe 0x401a50 entry and its return in opcode 0x407386, the
 * two text-character draws (0x4062ff ASCII, 0x40636d Big5), and the
 * wait-for-click at 0x406acc; 0x40616e is the script VM's opcode fetch
 * (logged with NANPA2_VMTRACE=1, watched by the exploration bot); 0x4071bb
 * is inside the script hit test 0x4071a4 with edi = its rectangle table
 * (NANPA2_HOTSPOTS=1; its C handler in src/game/ops.c fires it too). */
HLE_PROBE(0x40616e, "script VM opcode fetch")
HLE_PROBE(0x401a50, "wipe entry")
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
#ifdef HLE_OVERRIDES_UNDEF_P
#undef HLE_PROBE
#undef HLE_OVERRIDES_UNDEF_P
#endif
