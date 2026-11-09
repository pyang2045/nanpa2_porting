/* Live status file and watchdog, for diagnosing a running instance without
 * restarting it: about once a second the game writes
 * $TMPDIR/nanpa2-status-<pid>.json (removed on exit) with the window,
 * presenter, game timer and sound state, and logs one WARNING line when
 * the screen stays black, nothing reaches the window, or the game stops.
 * See README.md, "Diagnosing a running instance". */
#ifndef NANPA2_STATUS_H
#define NANPA2_STATUS_H

#include <SDL.h>

/* Start the writer thread before SDL_Init (phase "init": the file then
 * shows an instance stuck in the video setup). */
void status_early(void);
/* The startup stage reached ("sdl_init", "window", "present_init", ...),
 * shown in the file until the game runs (stage "running"). */
void status_stage(const char *name);
/* The game starts (starts the thread too if status_early was not called).
 * speed: the game speed setting (1.0 = the original's, 0 = unthrottled). */
void status_init(SDL_Window *win, double speed);
/* Main-thread heartbeat: cheap; refreshes the window and presenter numbers
 * a few times a second (they must be read on the main thread). */
void status_main(void);
void status_set_logo(int playing);
/* Stop the thread and remove the file (also at exit and on SIGTERM etc). */
void status_shutdown(void);

#endif
