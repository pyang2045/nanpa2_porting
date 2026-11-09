#include "game/ctl.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include "platform/window.h"
#include "platform/clock.h"

int ctl_pump(void)
{
	return win_pump();
}

void ctl_work_us(uint32_t us)
{
	clock_work(us);
}

void ctl_tlog(const char *label, uint32_t arg)
{
	static int on = -1;
	if (on < 0)
		on = getenv("NANPA2_TIMELOG") != NULL;
	if (!on)
		return;
	uint64_t ms = clock_log_ms();
	fprintf(stderr, "T %llu %u %s %x\n", (unsigned long long)ms, RT(frame), label, arg);
}

static jmp_buf *catch_top;

_Noreturn void ctl_quit(void)
{
	if (!catch_top)
		game_trap(0x40638d, "WM_QUIT with nothing to unwind to");
	longjmp(*catch_top, 1);
}

void ctl_pump_or_quit(void)
{
	if (!ctl_pump())
		ctl_quit();
}

int ctl_catch(void (*fn)(void *), void *arg)
{
	jmp_buf here, *outer = catch_top;
	if (setjmp(here)) {
		catch_top = outer;
		return 0;
	}
	catch_top = &here;
	fn(arg);
	catch_top = outer;
	return 1;
}
