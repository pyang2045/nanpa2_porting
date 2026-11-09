/*
 * Dev hooks: the one way platform code (src/platform/) and the trap code (src/trap.c) call into
 * the dev layer (src/dev/: deterministic mode, recording, scripted input, status file).
 *
 * The lower layers never include dev headers. They call the inline wrappers below, which do
 * nothing (or return 0) while a hook is NULL. main.c installs the dev layer's hooks once, before
 * anything else runs (dev_install_hooks, src/dev/dev.c); a build or a test that has no dev layer
 * simply leaves them unset.
 */
#ifndef NANPA2_HOOKS_H
#define NANPA2_HOOKS_H

#include <stdint.h>

struct dev_hooks {
	int (*det)(void);              /* deterministic clock (replay_det) */
	int (*replaying)(void);        /* host input is off (replay_replaying) */
	uint64_t (*virtual_ms)(void);  /* the virtual game clock in ms (replay_ms) */
	void (*pump)(void);            /* once per message pump, after the timer ran (replay_pump) */
	/* a message from outside the game; 0 = drop it (replay_external) */
	int (*external)(uint32_t msg, uint32_t wparam, uint32_t lparam);
	void (*file_opened)(const char *name);              /* a game data file was opened */
	void (*frame)(const uint8_t *frame, const uint32_t pal[256]); /* a frame was presented */
	void (*inject)(void);          /* scripted input and the bot post their messages */
	int (*scripted)(void);         /* NANPA2_INPUT given: host input is ignored */
	uint32_t (*now)(void);         /* wall-clock ms since the dev layer started */
	void (*beat)(void);            /* main-thread heartbeat (status file) */
	void (*logo)(int playing);     /* the start-up logo video starts (1) or ends (0) */
	void (*vm_fetch)(uint32_t off);    /* the script VM fetched the opcode at off */
	void (*hit_table)(uint32_t table); /* the hit test is about to scan the table */
};

extern struct dev_hooks dev_hooks;

static inline int hook_det(void) { return dev_hooks.det && dev_hooks.det(); }
static inline int hook_replaying(void) { return dev_hooks.replaying && dev_hooks.replaying(); }
static inline uint64_t hook_virtual_ms(void) { return dev_hooks.virtual_ms ? dev_hooks.virtual_ms() : 0; }
static inline void hook_pump(void) { if (dev_hooks.pump) dev_hooks.pump(); }
static inline int hook_external(uint32_t msg, uint32_t wp, uint32_t lp)
{
	return dev_hooks.external ? dev_hooks.external(msg, wp, lp) : 1;
}
static inline void hook_file_opened(const char *n) { if (dev_hooks.file_opened) dev_hooks.file_opened(n); }
static inline void hook_frame(const uint8_t *f, const uint32_t pal[256])
{
	if (dev_hooks.frame) dev_hooks.frame(f, pal);
}
static inline void hook_inject(void) { if (dev_hooks.inject) dev_hooks.inject(); }
static inline int hook_scripted(void) { return dev_hooks.scripted && dev_hooks.scripted(); }
static inline uint32_t hook_now(void) { return dev_hooks.now ? dev_hooks.now() : 0; }
static inline void hook_beat(void) { if (dev_hooks.beat) dev_hooks.beat(); }
static inline void hook_logo(int playing) { if (dev_hooks.logo) dev_hooks.logo(playing); }
static inline void hook_vm_fetch(uint32_t off) { if (dev_hooks.vm_fetch) dev_hooks.vm_fetch(off); }
static inline void hook_hit_table(uint32_t t) { if (dev_hooks.hit_table) dev_hooks.hit_table(t); }

#endif
