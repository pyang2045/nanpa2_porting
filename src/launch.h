/* Startup outside a terminal: the log file and the game folder. */
#ifndef NANPA2_LAUNCH_H
#define NANPA2_LAUNCH_H

/* Folder for the remembered game path and the log: NANPA2_SAVE_DIR if set
 * (tests), else SDL_GetPrefPath("nanpa2", "nanpa2"), the save folder (on
 * macOS ~/Library/Application Support/nanpa2/nanpa2). No trailing slash;
 * NULL if there is none. */
const char *launch_pref_dir(void);

/* 1 when running from a macOS .app bundle (Contents/MacOS). */
int launch_in_bundle(void);

/* Send stderr, stdout and SDL_Log to a file when nobody sees them: inside
 * an .app bundle (<pref>/nanpa2.log, the last run's kept as
 * nanpa2.previous.log), or always to NANPA2_LOG_FILE if set. Call first. */
void launch_log_init(int argc, char **argv);

/* The game folder (the CD contents, with FLAG0..FLAG6), first
 * that validates of: cli (--game), NANPA2_GAME_DIR, the path remembered in
 * <pref>/game-folder.txt, next to the executable or the .app (and in a
 * "game" folder there, up a few levels), ./game; then, if interactive, a
 * folder picker whose choice is remembered. cli and NANPA2_GAME_DIR are
 * final: if they fail, so does this. NULL: not found; the reason went to
 * stderr and, if interactive, a message box. */
const char *launch_game_dir(const char *cli, int interactive);

#endif
