/* Live status file and watchdog, for diagnosing a running instance without
 * restarting it: about once a second the game writes
 * $TMPDIR/nanpa2-status-<pid>.json (removed on exit) with the window,
 * presenter, game timer and sound state, and logs one WARNING line when
 * the screen stays black, nothing reaches the window, or the game stops.
 * See README.md, "Diagnosing a running instance". */
#ifndef NANPA2_STATUS_H
#define NANPA2_STATUS_H

#include <SDL.h>

/* Start the writer thread. mips: the throttle setting. */
void status_init(SDL_Window *win, double mips);
/* Main-thread heartbeat: cheap; refreshes the window and presenter numbers
 * a few times a second (they must be read on the main thread). */
void status_main(void);
void status_set_logo(int playing);
/* Stop the thread and remove the file (also at exit and on SIGTERM etc). */
void status_shutdown(void);

#endif
