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
HLE_GAME_OVERRIDE(0x4069fd, "VM number to text, for op 15 (src/game/ops_vm.c)")
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

/* Handlers that run a block, a script or a wait, split at those calls
 * (src/game/ops_vm.c): each call is a call exit, the code after it a
 * jump unit */
HLE_GAME_JUMP(0x40659e, "op 0f: if")
HLE_GAME_JUMP(0x4065ae, "op 0f: after the block of a true condition")
HLE_GAME_JUMP(0x4065ed, "op 0f: after the else block")
HLE_GAME_JUMP(0x4065f6, "op 11: wait for a click")
HLE_GAME_JUMP(0x4065fb, "op 11: after the wait, clear the text window")
HLE_GAME_JUMP(0x40660c, "op 04 10: while")
HLE_GAME_JUMP(0x40661e, "op 04 10: after the loop's block")
HLE_GAME_JUMP(0x406658, "op 13: call label")
HLE_GAME_JUMP(0x40666b, "op 13: after the label's block")
HLE_GAME_JUMP(0x406a9a, "op 14: call offset")
HLE_GAME_JUMP(0x406aa7, "op 14: after the script")
HLE_GAME_JUMP(0x406aad, "op 16: wait")
HLE_GAME_JUMP(0x406abd, "op 16: after the frames")
HLE_GAME_JUMP(0x406ac7, "op 16: after the click")
HLE_GAME_JUMP(0x406671, "op 04 13: menu loop")
HLE_GAME_JUMP(0x4066b8, "op 04 13: round start (pump)")
HLE_GAME_JUMP(0x4066bd, "op 04 13: after the pump")
HLE_GAME_JUMP(0x4066e6, "op 04 13: after the frame block, scan the inputs")
HLE_GAME_JUMP(0x4066f6, "op 04 13: after testing click")
HLE_GAME_JUMP(0x406704, "op 04 13: after the click block")
HLE_GAME_JUMP(0x406714, "op 04 13: after testing click for its release")
HLE_GAME_JUMP(0x406728, "op 04 13: after testing right click")
HLE_GAME_JUMP(0x406736, "op 04 13: after the right click block")
HLE_GAME_JUMP(0x40674a, "op 04 13: after testing right click for its release")
HLE_GAME_JUMP(0x406761, "op 04 13: after testing up")
HLE_GAME_JUMP(0x40676f, "op 04 13: after the up block")
HLE_GAME_JUMP(0x406783, "op 04 13: after testing up for its release")
HLE_GAME_JUMP(0x40679a, "op 04 13: after testing down")
HLE_GAME_JUMP(0x4067a8, "op 04 13: after the down block")
HLE_GAME_JUMP(0x4067bc, "op 04 13: after testing down for its release")
HLE_GAME_JUMP(0x4067d3, "op 04 13: after testing left")
HLE_GAME_JUMP(0x4067e1, "op 04 13: after the left block")
HLE_GAME_JUMP(0x4067f5, "op 04 13: after testing left for its release")
HLE_GAME_JUMP(0x406810, "op 04 13: after testing right")
HLE_GAME_JUMP(0x406822, "op 04 13: after the right block")
HLE_GAME_JUMP(0x40683b, "op 04 13: after testing right for its release")
HLE_GAME_JUMP(0x4069ce, "op 15: print a number")
HLE_GAME_JUMP(0x4069f7, "op 15: after the digits")
HLE_GAME_JUMP(0x406f7c, "op 04 1e: jump to a MES file")
HLE_GAME_JUMP(0x406fcd, "op 04 1f: call a MES file")
HLE_GAME_JUMP(0x40704a, "op 04 1f: after the called file")
HLE_GAME_JUMP(0x4070d5, "op 04 21: FLAG file")
HLE_GAME_CALL_EXIT(0x4065ae, "op 0f: run the block of a true condition (0x4061cd)")
HLE_GAME_CALL_EXIT(0x4065ed, "op 0f: run the else block (0x4061cd)")
HLE_GAME_CALL_EXIT(0x4065fb, "op 11: wait for a click (0x406acc)")
HLE_GAME_CALL_EXIT(0x40661e, "op 04 10: run the loop's block (0x4061cd)")
HLE_GAME_CALL_EXIT(0x40666b, "op 13: run the label's block (0x4061cd)")
HLE_GAME_CALL_EXIT(0x406aa7, "op 14: run the script at the offset (0x4060f9)")
HLE_GAME_CALL_EXIT(0x406abd, "op 16: wait n frames (0x4075c6)")
HLE_GAME_CALL_EXIT(0x406ac7, "op 16: wait for a click (0x406acc)")
HLE_GAME_CALL_EXIT(0x4066b8, "op 04 13: run label 38 (0x4061cd)")
HLE_GAME_CALL_EXIT(0x4066bd, "op 04 13: pump messages (0x401bf0)")
HLE_GAME_CALL_EXIT(0x4066e6, "op 04 13: run label 39 (0x4061cd)")
HLE_GAME_CALL_EXIT(0x4066f6, "op 04 13: test click (0x4074d5)")
HLE_GAME_CALL_EXIT(0x406704, "op 04 13: run label 32 (0x4061cd)")
HLE_GAME_CALL_EXIT(0x406714, "op 04 13: test click again (0x4074d5)")
HLE_GAME_CALL_EXIT(0x406728, "op 04 13: test right click (0x4074d5)")
HLE_GAME_CALL_EXIT(0x406736, "op 04 13: run label 33 (0x4061cd)")
HLE_GAME_CALL_EXIT(0x40674a, "op 04 13: test right click again (0x4074d5)")
HLE_GAME_CALL_EXIT(0x406761, "op 04 13: test up (0x4074d5)")
HLE_GAME_CALL_EXIT(0x40676f, "op 04 13: run label 34 (0x4061cd)")
HLE_GAME_CALL_EXIT(0x406783, "op 04 13: test up again (0x4074d5)")
HLE_GAME_CALL_EXIT(0x40679a, "op 04 13: test down (0x4074d5)")
HLE_GAME_CALL_EXIT(0x4067a8, "op 04 13: run label 35 (0x4061cd)")
HLE_GAME_CALL_EXIT(0x4067bc, "op 04 13: test down again (0x4074d5)")
HLE_GAME_CALL_EXIT(0x4067d3, "op 04 13: test left (0x4074d5)")
HLE_GAME_CALL_EXIT(0x4067e1, "op 04 13: run label 36 (0x4061cd)")
HLE_GAME_CALL_EXIT(0x4067f5, "op 04 13: test left again (0x4074d5)")
HLE_GAME_CALL_EXIT(0x406810, "op 04 13: test right (0x4074d5)")
HLE_GAME_CALL_EXIT(0x406822, "op 04 13: run label 37 (0x4061cd)")
HLE_GAME_CALL_EXIT(0x40683b, "op 04 13: test right again (0x4074d5)")
HLE_GAME_CALL_EXIT(0x4069f7, "op 15: run the digits as a script (0x4060f9)")
HLE_GAME_CALL_EXIT(0x40704a, "op 04 1f: run the called file (0x4060f9)")

/* Stage 4: the text, the animation slots and the palette copy
 * (src/game/text.c, anim.c); 0x403510 leaves sub-op 4 (run a slot to its
 * end, spinning on the frame counter) to the original */
HLE_GAME_OVERRIDE(0x401900, "draw one character (src/game/text.c)")
HLE_GAME_OVERRIDE(0x403624, "animation step, once a frame (src/game/anim.c)")
HLE_GAME_OVERRIDE(0x403661, "run one animation slot (src/game/anim.c)")
HLE_GAME_OVERRIDE(0x4063ee, "text cursor advance (src/game/text.c)")
HLE_GAME_OVERRIDE(0x406b24, "clear the text window (src/game/text.c)")
HLE_GAME_OVERRIDE(0x406dff, "reset the animation slots (src/game/anim.c)")
HLE_GAME_OVERRIDE(0x40752c, "copy a picture palette (src/game/anim.c)")

/* Stage 4: the VM loop and the waits (src/game/loop.c), split at the calls
 * that pump messages, wait or run a block like the handlers above. They
 * are entered by call too (0x4060d4 from WinMain, 0x4061cd, 0x4074d5,
 * 0x4075c6 and 0x406acc from the handlers): recomp.py sends such calls
 * through dispatch. The click handler path after 0x406124/0x406134
 * (0x40613f-0x40616c) stays native with ext 20. */
HLE_GAME_JUMP(0x4060d4, "VM entry: set up, load the MES file")
HLE_GAME_JUMP(0x4060f9, "VM loop head (pump)")
HLE_GAME_JUMP(0x4060fe, "VM loop: after the pump, animate, test the left button or fetch")
HLE_GAME_JUMP(0x406124, "VM loop: after testing the left button")
HLE_GAME_JUMP(0x406134, "VM loop: after testing the right button")
HLE_GAME_JUMP(0x4061c2, "VM: after a block, skip it")
HLE_GAME_JUMP(0x4061cd, "VM: run a block (nested loop)")
HLE_GAME_JUMP(0x4061d3, "VM: return from a block")
HLE_GAME_JUMP(0x406315, "VM text: after an ASCII character, the next")
HLE_GAME_JUMP(0x406383, "VM text: after a Big5 character")
HLE_GAME_JUMP(0x4074d5, "input test: pump")
HLE_GAME_JUMP(0x4074db, "input test: after the pump, test the input")
HLE_GAME_JUMP(0x4075c6, "frame wait: pump")
HLE_GAME_JUMP(0x4075d7, "frame wait: after the pump, animate, wait on")
HLE_GAME_JUMP(0x406acc, "click wait: Ctrl, or a frame")
HLE_GAME_JUMP(0x406adf, "click wait: after the frame, test")
HLE_GAME_JUMP(0x406ae6, "click wait: after the test")
HLE_GAME_JUMP(0x406afb, "click wait for the release: after the frame, test")
HLE_GAME_JUMP(0x406b02, "click wait for the release: after the test")
HLE_GAME_CALL_EXIT(0x4060fe, "VM loop: pump messages (0x401bf0)")
HLE_GAME_CALL_EXIT(0x406124, "VM loop: test the left button (0x4074d5)")
HLE_GAME_CALL_EXIT(0x406134, "VM loop: test the right button (0x4074d5)")
HLE_GAME_CALL_EXIT(0x4061c2, "VM: run the block at an 01 (0x4061cd)")
HLE_GAME_CALL_EXIT(0x4061d3, "VM: the block as a nested loop (0x4060f9)")
HLE_GAME_CALL_EXIT(0x406315, "VM text: the character delay (0x4075c6)")
HLE_GAME_CALL_EXIT(0x406383, "VM text: the Big5 character delay (0x4075c6)")
HLE_GAME_CALL_EXIT(0x4074db, "input test: pump messages (0x401bf0)")
HLE_GAME_CALL_EXIT(0x4075d7, "frame wait: pump messages (0x401bf0)")
HLE_GAME_CALL_EXIT(0x406adf, "click wait: a frame (0x4075c6)")
HLE_GAME_CALL_EXIT(0x406ae6, "click wait: test the button (0x4074d5)")
HLE_GAME_CALL_EXIT(0x406afb, "click wait for the release: a frame (0x4075c6)")
HLE_GAME_CALL_EXIT(0x406b02, "click wait for the release: test (0x4074d5)")

/* Stage 5B (src/game/anim.c, ops_sys.c): the slot control 0x403510 is a
 * jump unit entered by the call of op 1a (call exit 0x4070b6); sub-op 4
 * goes on in the wait loop 0x4035b5, a piece that returns itself while it
 * waits (game_jump polls for it, as the native backward jump does) */
HLE_GAME_JUMP(0x403510, "animation slot control (src/game/anim.c)")
HLE_GAME_JUMP(0x4035b5, "animation slot wait loop (src/game/anim.c)")
HLE_GAME_JUMP(0x4070b6, "op 1a: after the slot control (src/game/ops_sys.c)")
HLE_GAME_CALL_EXIT(0x4070b6, "op 1a: slot control (0x403510)")

/* Stage 5B startup/input/file (src/game/file.c): the file routines, called
 * by the C pieces directly and by native code (the map module, WinMain) */
HLE_GAME_OVERRIDE(0x401000, "open a game file (src/game/file.c)")
HLE_GAME_OVERRIDE(0x401050, "load a game file (src/game/file.c)")
HLE_GAME_OVERRIDE(0x401160, "save a file (src/game/file.c)")

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
#ifdef HLE_OVERRIDES_UNDEF_C
#undef HLE_GAME_CALL_EXIT
#undef HLE_OVERRIDES_UNDEF_C
#endif
#ifdef HLE_OVERRIDES_UNDEF_P
#undef HLE_PROBE
#undef HLE_OVERRIDES_UNDEF_P
#endif
