/* Startup outside a terminal (launch.h): a launch from Finder has no
 * arguments, / as the working folder and stderr going nowhere, so the log
 * goes to a file and the game folder is looked up, or asked for once. */
#include "launch.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fsys.h"
#include "platform/dialog.h"
#include "platform/docs.h"

#define PATH_CAP 1024

#ifdef __IPHONEOS__
#define UNUSED_ON_IOS __attribute__((unused))
#else
#define UNUSED_ON_IOS
#endif

static char pref[PATH_CAP];
static int pref_done;

const char *launch_pref_dir(void)
{
	if (!pref_done) {
		pref_done = 1;
		const char *env = getenv("NANPA2_SAVE_DIR");
		char *sdl = env ? NULL : SDL_GetPrefPath("nanpa2", "nanpa2");
		snprintf(pref, sizeof pref, "%s", env ? env : sdl ? sdl : "");
		SDL_free(sdl);
		size_t n = strlen(pref);
		while (n > 1 && (pref[n - 1] == '/' || pref[n - 1] == '\\'))
			pref[--n] = 0;
	}
	return pref[0] ? pref : NULL;
}

int launch_in_bundle(void)
{
	static int known = -1;
	if (known < 0) {
		char *base = SDL_GetBasePath(); /* bundles: .../X.app/Contents/Resources/ */
		known = base && strstr(base, ".app/Contents/") != NULL;
		SDL_free(base);
#ifdef __IPHONEOS__
		known = 1; /* an iOS app has no console: the log goes to the save folder */
#endif
	}
	return known;
}

/* ---- log file ---- */

static FILE *log_file;

static void log_output(void *ud, int category, SDL_LogPriority priority, const char *msg)
{
	static const char *const names[] = { "", "VERBOSE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL" };
	(void)ud;
	(void)category;
	const char *p = priority > 0 && priority < (int)(sizeof names / sizeof names[0]) ? names[priority] : "";
	fprintf(log_file, "%7.3f %s: %s\n", SDL_GetTicks() / 1000.0, p, msg);
	fflush(log_file);
}

void launch_log_init(int argc, char **argv)
{
	const char *env = getenv("NANPA2_LOG_FILE");
	char path[PATH_CAP + 32], prev[PATH_CAP + 32];
	if (env && *env)
		snprintf(path, sizeof path, "%s", env);
	else if (launch_in_bundle() && launch_pref_dir()) {
		snprintf(path, sizeof path, "%s/nanpa2.log", launch_pref_dir());
		snprintf(prev, sizeof prev, "%s/nanpa2.previous.log", launch_pref_dir());
		remove(prev);
		rename(path, prev);
	} else
		return;
	/* stdout and stderr both append to the one file, so lines interleave */
	if (!freopen(path, "w", stderr))
		return;
	setvbuf(stderr, NULL, _IONBF, 0);
	if (freopen(path, "a", stdout))
		setvbuf(stdout, NULL, _IOLBF, 0);
	log_file = stderr;
	SDL_LogSetOutputFunction(log_output, NULL);
	time_t now = time(NULL);
	char when[64];
	strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S %z", localtime(&now));
	fprintf(stderr, "nanpa2 started %s, SDL %d.%d.%d\n", when, SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
		SDL_PATCHLEVEL);
	char *base = SDL_GetBasePath();
	fprintf(stderr, "executable folder: %s\narguments:", base ? base : "?");
	SDL_free(base);
	for (int i = 1; i < argc; i++)
		fprintf(stderr, " %s", argv[i]);
	fprintf(stderr, "\n");
}

/* ---- game folder ---- */

static char found[PATH_CAP];
static char tried[4096]; /* candidates that failed, for the not-found report */

/* What dir lacks, or NULL if it looks like the game folder. Uses fsys, so
 * names match case-insensitively as the game's own loads do. */
static const char *check(const char *dir)
{
	if (!dir || !*dir)
		return "no folder given";
	fsys_set_root(dir);
	FILE *f = fsys_open("DATA\\01.GP4");
	if (!f)
		return "no DATA folder with the pictures (DATA/01.GP4)";
	fclose(f);
	return NULL;
}

static int try_dir(const char *dir, const char *source)
{
	const char *why = check(dir);
	if (why) {
		size_t n = strlen(tried);
		snprintf(tried + n, sizeof tried - n, "  %s (%s): %s\n", dir, source, why);
		return 0;
	}
	snprintf(found, sizeof found, "%s", dir);
	SDL_Log("game folder: %s (%s)", found, source);
	return 1;
}

static void remembered_path(char *out, size_t cap)
{
	snprintf(out, cap, "%s/game-folder.txt", launch_pref_dir() ? launch_pref_dir() : ".");
}

UNUSED_ON_IOS static int read_remembered(char *out, size_t cap)
{
	char p[PATH_CAP + 32];
	remembered_path(p, sizeof p);
	FILE *f = fopen(p, "rb");
	if (!f)
		return 0;
	int ok = fgets(out, (int)cap, f) != NULL;
	fclose(f);
	if (ok)
		out[strcspn(out, "\r\n")] = 0;
	return ok && *out;
}

UNUSED_ON_IOS static void write_remembered(const char *dir)
{
	if (!launch_pref_dir())
		return;
	char p[PATH_CAP + 32];
	remembered_path(p, sizeof p);
	FILE *f = fopen(p, "wb");
	if (!f) {
		SDL_Log("cannot write %s", p);
		return;
	}
	fprintf(f, "%s\n", dir);
	fclose(f);
	SDL_Log("remembered the game folder in %s", p);
}

static void strip_last(char *p)
{
	size_t n = strlen(p);
	while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\'))
		p[--n] = 0;
	while (n > 0 && p[n - 1] != '/' && p[n - 1] != '\\')
		p[--n] = 0;
	while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\'))
		p[--n] = 0;
}

/* The executable's folder and a few above it (an .app's Resources,
 * Contents, the .app, the folder holding it, one more: so build/X.app finds
 * the repository's game/), each as itself and with "game" appended. */
UNUSED_ON_IOS static int try_near_exe(void)
{
	char *base = SDL_GetBasePath();
	if (!base)
		return 0;
	char dir[PATH_CAP], cand[PATH_CAP + 8];
	snprintf(dir, sizeof dir, "%s", base);
	SDL_free(base);
	int levels = launch_in_bundle() ? 5 : 2;
	for (int i = 0; i < levels && dir[0]; i++) {
		size_t n = strlen(dir);
		while (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\'))
			dir[--n] = 0;
		/* not the bundle's own folders */
		if ((!launch_in_bundle() || i >= 3) && try_dir(dir, "next to the executable"))
			return 1;
		snprintf(cand, sizeof cand, "%s/game", dir);
		if (try_dir(cand, "next to the executable"))
			return 1;
		strip_last(dir);
	}
	return 0;
}

static void message(const char *text)
{
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "同級生２", text, NULL);
}

/* Folder picker until a folder validates or the player gives up. */
UNUSED_ON_IOS static int pick(void)
{
	if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) /* SDL sets up the app first */
		return 0;
	const char *prompt = "請選擇同級生２的遊戲資料夾（含 DATA）\n"
			     "Choose the 同級生２ game folder (with DATA)";
	char dir[PATH_CAP];
	while (dialog_pick_folder(prompt, dir, sizeof dir)) {
		const char *why = check(dir);
		if (!why) {
			snprintf(found, sizeof found, "%s", dir);
			SDL_Log("game folder: %s (chosen)", found);
			write_remembered(found);
			return 1;
		}
		char text[PATH_CAP + 400];
		snprintf(text, sizeof text,
			 "這個資料夾不是遊戲資料夾。\nThis is not the game folder:\n\n%s\n\n%s", dir, why);
		SDL_Log("chosen folder %s: %s", dir, why);
		const SDL_MessageBoxButtonData buttons[] = {
			{ SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "選擇其他資料夾 Choose another" },
			{ SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "結束 Quit" },
		};
		const SDL_MessageBoxData box = { SDL_MESSAGEBOX_ERROR, NULL, "同級生２", text, 2, buttons, NULL };
		int b = 0;
		if (SDL_ShowMessageBox(&box, &b) < 0 || b != 1)
			return 0;
	}
	return 0;
}

#ifdef __IPHONEOS__
/* iOS/iPadOS: the game folder is copied into the app's Documents (Finder file sharing or the Files
 * app), either as a folder (Documents/game) or as its contents (Documents). */
static const char *ios_game_dir(int interactive)
{
	const char *docs = docs_dir();
	char cand[PATH_CAP + 8];
	if (docs) {
		snprintf(cand, sizeof cand, "%s/game", docs);
		if (try_dir(cand, "Documents") || try_dir(docs, "Documents"))
			return found;
	}
	char text[PATH_CAP + 900];
	int n = snprintf(text, sizeof text,
			 "找不到遊戲資料。請用 Finder（USB 連接）或「檔案」App，把遊戲資料夾"
			 "（光碟內容加上 data1.cab 內的 FLAG0..FLAG6，內含 DATA 資料夾）複製到"
			 "「在我的 iPad 上 > 同級生２」，然後重新開啟本程式。\n\n"
			 "The game data was not found. Copy the game folder (the CD contents plus FLAG0..FLAG6 "
			 "from data1.cab, with DATA inside) into this app's Documents through Finder (USB) or "
			 "the Files app (On My iPad > 同級生２), then relaunch.");
	if (launch_pref_dir() && n > 0 && (size_t)n < sizeof text)
		snprintf(text + n, sizeof text - (size_t)n, "\n\nLog: %s/nanpa2.log", launch_pref_dir());
	fprintf(stderr, "%s\ntried:\n%s", text, tried);
	if (interactive && SDL_InitSubSystem(SDL_INIT_VIDEO) == 0)
		message(text);
	return NULL;
}
#endif

const char *launch_game_dir(const char *cli, int interactive)
{
	const char *env = getenv("NANPA2_GAME_DIR");
	const char *given = cli ? cli : env && *env ? env : NULL;
	if (given) {
		const char *why = check(given);
		if (!why) {
			snprintf(found, sizeof found, "%s", given);
			SDL_Log("game folder: %s (%s)", found, cli ? "--game" : "NANPA2_GAME_DIR");
			return found;
		}
		char text[PATH_CAP + 400];
		snprintf(text, sizeof text, "找不到遊戲資料。\nThe game folder %s (%s) is not usable:\n%s", given,
			 cli ? "--game" : "NANPA2_GAME_DIR", why);
		fprintf(stderr, "%s\n", text);
		if (interactive)
			message(text);
		return NULL;
	}
#ifdef __IPHONEOS__
	return ios_game_dir(interactive);
#else
	char mem[PATH_CAP];
	if (read_remembered(mem, sizeof mem)) {
		if (try_dir(mem, "remembered"))
			return found;
		SDL_Log("the remembered game folder %s is not usable: %s", mem, check(mem));
	}
	if (try_near_exe() || try_dir("game", "working folder"))
		return found;
	if (interactive && pick())
		return found;
	char text[PATH_CAP + 400];
	int n = snprintf(text, sizeof text,
			 "找不到遊戲資料。\nThe game folder was not found. It is the CD contents with "
			 "FLAG0..FLAG6 from data1.cab.\n\n"
			 "Choose it when asked, put it next to the app (in a folder named \"game\"), "
			 "or start with --game DIR or NANPA2_GAME_DIR.");
	if (log_file && launch_pref_dir() && n > 0 && (size_t)n < sizeof text)
		snprintf(text + n, sizeof text - (size_t)n, "\n\nLog: %s/nanpa2.log", launch_pref_dir());
	fprintf(stderr, "%s\ntried:\n%s", text, tried);
	if (interactive)
		message(text);
	return NULL;
#endif
}
