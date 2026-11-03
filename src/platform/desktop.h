/* Whether anyone can see the desktop: screen lock and display sleep, for the
 * live status file (src/hle/status.c) and the smoke test. macOS:
 * desktop_macos.c (CoreGraphics); elsewhere unknown. Safe to call from any
 * thread; it is a WindowServer round trip, so call it about once a second. */
#ifndef NANPA2_DESKTOP_H
#define NANPA2_DESKTOP_H

/* *locked: the login session's screen is locked; *asleep: the main display
 * sleeps. 1 yes, 0 no, -1 unknown. */
#ifdef __APPLE__
void desktop_state(int *locked, int *asleep);
#else
static inline void desktop_state(int *locked, int *asleep)
{
	*locked = *asleep = -1;
}
#endif

#endif
