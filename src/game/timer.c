/*
 * The 16 ms timer tick (native 0x4028c0), the game's clock. Steps a palette fade (start colours
 * G.sc.pal_start, deltas G.sc.pal_delta, step G.sc.fade_step of 16, every G.rt.fade_steps ticks),
 * counts frames in G.rt.frame, and blinks the software cursor every 30 ticks. The platform runs
 * it (platform/window.h) once for every 16 ms the game clock has passed.
 */
#include "game/startup.h"

#include "game/ctl.h"
#include "platform/surface.h"
#include "platform/window.h"

void game_timer_tick(void)
{
	static uint32_t step_logged_start = 0xffffffffu;
	if (!SC(active))
		return;
	if (SC(fade_active)) {
		if (step_logged_start != RT(frame) && SC(fade_step) == 0 && SC(fade_wait) == 0)
			game_probe(0x4028c0); /* fade starts (probe log) */
		step_logged_start = RT(frame);
		uint32_t wait = SC(fade_wait) + 1;
		SC_SET(fade_wait, wait);
		if ((int32_t)wait >= (int32_t)RT(fade_steps)) {
			SC_SET(fade_wait, 0);
			const float k = 1.0f / 16; /* 1 / the 16 steps of a fade (0x41b188) */
			double f = (double)(int32_t)SC(fade_step) * k;
			for (uint32_t i = 0; i < 256; i++) {
				for (uint32_t c = 0; c < 3; c++) {
					int32_t delta = G.sc.pal_delta[i][c];
					double v = delta * f + G.sc.pal_start[4 * i + c];
					G.sc.pal_entries[4 * i + c] = (uint8_t)(int32_t)v; /* _ftol truncates */
				}
			}
			pal_set(0, 256, G.sc.pal_entries);
		}
		uint32_t step = SC(fade_step) + 1;
		SC_SET(fade_step, step);
		if ((int32_t)step > 16) {
			SC_SET(fade_wait, 0);
			SC_SET(fade_step, 0);
			SC_SET(fade_active, 0);
			ctl_tlog("fade-", 0);
			game_probe(0x4028c1); /* fade ends (probe log) */
		}
	}
	uint32_t frame = RT(frame) + 1;
	RT_SET(frame, frame);
	if (SC(cursor_on) && (int32_t)frame % 30 == 0) {
		SC_SET(cursor_frame, SC(cursor_frame) ^ 1);
		win_post(WM_MOUSEMOVE, 0, (SC(cursor_y) << 16) + SC(cursor_x));
	}
}
