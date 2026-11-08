/* Stage 5B: start-up, shutdown and the window procedure (src/game/startup.c) */
#ifndef NANPA2_GAME_STARTUP_H
#define NANPA2_GAME_STARTUP_H

#include <stdint.h>

uint32_t game_cd_search(void);     /* 0x402590 */
uint32_t game_display_init(void);  /* 0x402620 */
void game_shutdown(void);          /* 0x402a20 */
uint32_t game_winmain(void);       /* 0x402090, a jump unit */
uint32_t game_winmain_done(void);  /* 0x4021a3 */
void game_wndproc(void);           /* 0x4021c0 */

/* 1 while the harness runs the reimplementation after the original (their
 * effects on the host happened once already) */
int game_replaying(void);
extern int game_replay_pass; /* set by game.c around that run */

#endif
