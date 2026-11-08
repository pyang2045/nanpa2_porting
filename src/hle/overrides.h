/*
 * The one list of native game functions replaced by hand-written code, and
 * of the timing probes. Both backends read it: the interpreter
 * (src/x86/interp.c) includes it, and tools/recomp.py parses the
 * HLE_OVERRIDE / HLE_PROBE lines (keep one entry per line, hex address
 * first). Calls to and dispatches into an override address go to
 * hle_call(), which runs the replacement (src/hle/hle.c override_call or a
 * module it calls).
 *
 * Use as an X-macro: define HLE_OVERRIDE(addr, what) and/or
 * HLE_PROBE(addr, what), then include this file. No include guard.
 */
#ifndef HLE_OVERRIDE
#define HLE_OVERRIDE(addr, what)
#define HLE_OVERRIDES_UNDEF_O
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

/* Timing probes (x86_probe, logged with NANPA2_PROBE=1), run before these
 * instructions: wipe 0x401a50 entry and its return in opcode 0x407386, the
 * two text-character draws (0x4062ff ASCII, 0x40636d Big5), and the
 * wait-for-click at 0x406acc; 0x40616e is the script VM's opcode fetch
 * (logged with NANPA2_VMTRACE=1, watched by the exploration bot); 0x4071bb
 * is inside the script hit test 0x4071a4 with edi = its rectangle table
 * (NANPA2_HOTSPOTS=1). */
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
#ifdef HLE_OVERRIDES_UNDEF_P
#undef HLE_PROBE
#undef HLE_OVERRIDES_UNDEF_P
#endif
