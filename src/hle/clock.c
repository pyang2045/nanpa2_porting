#include "clock.h"

#include <SDL.h>

#include "hle.h"
#include "platform/present.h"
#include "replay.h"
#include "status.h"

static double speed = 1.0;
static uint64_t vclock;           /* virtual microseconds */
static uint64_t base_us;          /* throttle: the virtual time at base_ticks */
static uint64_t base_ticks;       /* SDL performance counter */

void clock_init(double s)
{
	speed = s < 0 ? 0 : s;
	base_us = vclock;
	base_ticks = SDL_GetPerformanceCounter();
}

double clock_speed(void)
{
	return speed;
}

uint64_t clock_us(void)
{
	return vclock;
}

void clock_add(uint32_t us)
{
	vclock += us;
}

uint64_t clock_timer_ms(void)
{
	if (replay_det() || speed <= 0)
		return vclock / 1000;
	return (uint64_t)((double)SDL_GetTicks() * speed);
}

/* Budget: the virtual time since the base point may not exceed the real time elapsed times the
 * speed. Ahead of it, sleep; far behind (HLE work, a stall, an unthrottled stretch), move the base
 * point so the game never bursts to catch up. Waits on the game timer are unaffected: the pumps
 * that spin on them cost virtual time too, and the timer itself follows real time. */
void clock_throttle(void)
{
	if (speed <= 0 || replay_replaying())
		return;
	uint64_t now = SDL_GetPerformanceCounter();
	double real = (double)(now - base_ticks) / (double)SDL_GetPerformanceFrequency();
	double virt = (double)(vclock - base_us) / 1e6 / speed;
	double ahead = virt - real;
	if (ahead >= 0.002) {
		SDL_Delay((uint32_t)(ahead * 1000.0));
	} else if (ahead < -0.05) {
		base_ticks = now;
		base_us = vclock;
	}
}

void clock_work(uint32_t us)
{
	static uint32_t last;
	vclock += us;
	clock_throttle();
	win32_run_timer();
	uint32_t now = SDL_GetTicks();
	if (now - last >= 8) { /* the host's own service, as the timer thread and DirectDraw did */
		last = now;
		status_main();
		ddraw_present(0);
		SDL_PumpEvents();
	}
}
