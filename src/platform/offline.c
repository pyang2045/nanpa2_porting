/* Platform API: the offline mode. See offline.h. */
#include "platform/offline.h"

#include "platform/clock.h"
#include "platform/surface.h"
#include "platform/text.h"
#include "platform/window.h"

static int on, ff = 1;

int offline_on(void)
{
	return on;
}

int offline_fast_forward(void)
{
	return on && ff;
}

void offline_set_fast_forward(int v)
{
	ff = v;
}

void offline_begin(void)
{
	on = 1;
	clock_reset();
	clock_init(0);
}

void offline_end(void)
{
	on = 0;
	win_reset();
	surf_reset();
	text_fonts_reset();
	clock_reset();
}
