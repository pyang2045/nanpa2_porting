/* Live status file and watchdog (status.h).
 *
 * The main thread refreshes a snapshot of what only it may read (SDL window
 * state, presenter counters) a few times a second; a writer thread turns it,
 * together with the game's own state read from emulated memory, into
 * $TMPDIR/nanpa2-status-<pid>.json once a second and runs the watchdog. The
 * thread keeps writing if the main thread hangs, so a stall shows up too.
 *
 * Game state: frame counter 0x446292 (16 ms timer 0x4028c0), active flag
 * 0x4ac4ec (WM_ACTIVATEAPP; the timer does nothing while it is 0), the last
 * X86_POLL pc.
 *
 * status_early() starts the thread before SDL_Init, so an instance that
 * hangs in the video setup (seen with the screen locked: the main thread
 * waits in CVDisplayLinkCreateWithCGDisplay / IOServiceOpen inside
 * SDL_Init) still writes a file: phase "init", the desktop state and a
 * warning. The game sections follow once status_init() has run. */
#include "status.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include "dev.h"
#include "hle.h"
#include "platform/desktop.h"
#include "platform/present.h"

#ifdef _WIN32
#define environ _environ /* stdlib.h */
#else
extern char **environ;
#endif

/* the screen may be black or unpresented this long (fades, the gap after
 * the logo movie) before the watchdog warns */
#define WATCH_MS 8000
/* the frame counter may stand still this long with the timer on */
#define WATCH_FRAME_MS 3000
#define WATCH_MAIN_MS 3000

static SDL_Window *window;
static double mips;
static char path[1024], tmp_path[1040];
static SDL_Thread *thread;
static SDL_mutex *lock;
static SDL_atomic_t quit;
static time_t start_wall;
static uint32_t launch_ticks; /* status_early() */
static uint32_t start_ticks;  /* status_init(): the game starts */
static SDL_atomic_t phase;    /* 0 before status_init, 1 game */
static const char *volatile stage = "start"; /* status_stage() */
static int headless, scripted;
static char env_json[2048];
static SDL_atomic_t logo;

/* filled by the main thread under lock */
static struct snap {
	uint32_t tick; /* SDL_GetTicks() of the last refresh */
	int windows;   /* SDL windows */
	uint32_t flags;
	int w, h, x, y;
	int focus;
	struct present_stats ps;
} snap;

/* watchdog state (writer thread only) */
static char warning[256], last_key[64], warning_key[64];
static char warned[512] = "[]"; /* every key warned about, a JSON array */
static unsigned warnings;
static uint32_t last_frame_value, last_frame_change;

static void json_str(char *out, size_t cap, const char *s)
{
	size_t o = 0;
	for (; *s && o + 7 < cap; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			out[o++] = '\\';
			out[o++] = (char)c;
		} else if (c < 0x20) {
			o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
		} else {
			out[o++] = (char)c;
		}
	}
	out[o] = 0;
}

/* NANPA2_* variables: scripted and headless test runs are told apart at once */
static void collect_env(void)
{
	size_t o = 0;
	o += (size_t)snprintf(env_json + o, sizeof env_json - o, "{");
	int first = 1;
	for (char **e = environ; *e; e++) {
		if (strncmp(*e, "NANPA2_", 7))
			continue;
		const char *eq = strchr(*e, '=');
		if (!eq)
			continue;
		char k[64], v[512], kq[160], vq[1100];
		snprintf(k, sizeof k, "%.*s", (int)(eq - *e) < 63 ? (int)(eq - *e) : 63, *e);
		snprintf(v, sizeof v, "%s", eq + 1);
		json_str(kq, sizeof kq, k);
		json_str(vq, sizeof vq, v);
		if (o + strlen(kq) + strlen(vq) + 8 >= sizeof env_json)
			break;
		o += (size_t)snprintf(env_json + o, sizeof env_json - o, "%s\"%s\": \"%s\"", first ? "" : ", ",
				      kq, vq);
		first = 0;
	}
	snprintf(env_json + o, sizeof env_json - o, "}");
}

static void warn(const char *key, const char *fmt, ...)
{
	char msg[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	/* log once per cause (the key), not every second */
	if (strcmp(last_key, key)) {
		snprintf(last_key, sizeof last_key, "%s", key);
		SDL_Log("WARNING: %s", msg);
		warnings++;
		char q[80];
		snprintf(q, sizeof q, "\"%s\"", key);
		size_t n = strlen(warned);
		if (!strstr(warned, q) && n + strlen(q) + 3 < sizeof warned)
			snprintf(warned + n - 1, sizeof warned - (n - 1), "%s%s]", n > 2 ? ", " : "", q);
	}
	snprintf(warning, sizeof warning, "%s", msg);
	snprintf(warning_key, sizeof warning_key, "%s", key);
}

/* The watchdog: the most likely cause of a black or frozen window. */
static void watchdog(const struct snap *s, uint32_t now, uint32_t frame, uint32_t active, uint32_t pc)
{
	const struct present_stats *ps = &s->ps;
	uint32_t up = now - start_ticks;
	static unsigned long seen_occl, seen_drawable, seen_size, seen_in;
	static uint32_t seen_at;
	/* counter rises over the last watch period */
	unsigned long d_occl = ps->skip_occluded - seen_occl, d_drawable = ps->skip_drawable - seen_drawable,
		      d_size = ps->skip_size - seen_size, d_in = ps->frames_in - seen_in;
	if (now - seen_at >= WATCH_MS) {
		seen_occl = ps->skip_occluded;
		seen_drawable = ps->skip_drawable;
		seen_size = ps->skip_size;
		seen_in = ps->frames_in;
		seen_at = now;
	}
	if (frame != last_frame_value) {
		last_frame_value = frame;
		last_frame_change = now;
	}
	warning[0] = warning_key[0] = 0;
	if (up < WATCH_MS)
		return;
	if (now - s->tick > WATCH_MAIN_MS) {
		warn("main", "main thread stalled for %u ms (last X86_POLL pc %08x, worst nextDrawable wait %.0f ms)",
		     now - s->tick, pc, ps->worst_wait_ms);
		return;
	}
	int timer = win32_timer_running(), in_logo = SDL_AtomicGet(&logo);
	if (timer && !in_logo && now - last_frame_change > WATCH_FRAME_MS && (headless || ps->occluded != 1)) {
		if (!active)
			warn("timer", "timer stopped: 0x4ac4ec=0 (game deactivated), frame counter %u for %u ms", frame,
			     now - last_frame_change);
		else
			warn("frame", "frame counter stuck at %u for %u ms with 0x4ac4ec=%u: stuck polling at pc %08x",
			     frame, now - last_frame_change, active, pc);
		return;
	}
	if (headless) {
		/* nothing is presented on purpose, so no window may show */
		if (ps->os_windows > 0)
			warn("headless", "headless run (NANPA2_HEADLESS) with %d window(s) on screen: they stay black",
			     ps->os_windows);
		return;
	}
	uint32_t since_present = ps->last_present ? now - ps->last_present : up;
	if (since_present > WATCH_MS) {
		if (!d_in)
			warn("noin", "no frame presented for %u ms: the presenter is not called (last X86_POLL pc %08x)",
			     since_present, pc);
		else if (d_occl)
			warn("occl", "no frame presented for %u ms: presents skipped, occluded=%lu (window covered or "
			     "hidden; os_windows %d)", since_present, ps->skip_occluded, ps->os_windows);
		else if (d_drawable)
			warn("drawable", "no frame presented for %u ms: presents skipped, no drawable=%lu (worst wait "
			     "%.0f ms)", since_present, ps->skip_drawable, ps->worst_wait_ms);
		else if (d_size)
			warn("size", "no frame presented for %u ms: presents skipped, drawable size 0 (%dx%d)",
			     since_present, ps->drawable_w, ps->drawable_h);
		else
			warn("present", "no frame presented for %u ms", since_present);
		return;
	}
	uint32_t black = ps->last_nonblack ? now - ps->last_nonblack : up;
	if (black > WATCH_MS) {
		if (ps->luma_in >= PRESENT_BLACK_LUMA)
			warn("stale", "screen black for %u ms although the game draws (luma %d): presents skipped "
			     "occluded=%lu drawable=%lu size=%lu", black, ps->luma_in, ps->skip_occluded,
			     ps->skip_drawable, ps->skip_size);
		else if (in_logo)
			warn("logo", "screen black for %u ms during the logo movie", black);
		else
			warn("black", "screen black for %u ms: the game draws black (frame %u, 0x4ac4ec=%u, pc %08x)",
			     black, frame, active, pc);
		return;
	}
}

static void write_json(const char *text)
{
	FILE *f = fopen(tmp_path, "w");
	if (!f)
		return;
	fputs(text, f);
	fclose(f);
	rename(tmp_path, path);
}

/* Before status_init: no window, presenter or game memory yet. */
static void write_init_status(uint32_t now, int locked, int asleep)
{
	uint32_t up = now - launch_ticks;
	warning[0] = warning_key[0] = 0;
	if (up > WATCH_MAIN_MS)
		warn("init", "startup stuck in stage %s for %u ms%s", stage, up,
		     locked == 1 ? " with the screen locked (display or GPU setup waits for the unlock; "
				   "NANPA2_HEADLESS runs avoid both)" : "");
	if (!warning[0])
		last_key[0] = 0;
	char wq[600], started[32], buf[4096];
	json_str(wq, sizeof wq, warning);
	strftime(started, sizeof started, "%Y-%m-%dT%H:%M:%SZ", gmtime(&start_wall));
	snprintf(buf, sizeof buf,
		 "{\n"
		 "  \"pid\": %d, \"started\": \"%s\", \"uptime_ms\": %u, \"stage\": \"%s\",\n"
		 "  \"mode\": {\"headless\": %d, \"scripted\": -1, \"env\": %s},\n"
		 "  \"desktop\": {\"locked\": %d, \"asleep\": %d, \"app_hidden\": -1},\n"
		 "  \"warning\": \"%s\", \"warning_key\": \"%s\", \"warnings\": %u, \"warned\": %s\n"
		 "}\n",
		 (int)getpid(), started, up, stage, headless, env_json, locked, asleep, wq, warning_key, warnings, warned);
	write_json(buf);
}

static void write_status(void)
{
	uint32_t now = SDL_GetTicks();
	int locked, asleep;
	desktop_state(&locked, &asleep);
	if (!SDL_AtomicGet(&phase)) {
		write_init_status(now, locked, asleep);
		return;
	}
	struct snap s;
	SDL_LockMutex(lock);
	s = snap;
	SDL_UnlockMutex(lock);
	now = SDL_GetTicks(); /* not before the snapshot (s.tick <= now) */
	uint32_t frame = rd32(0x446292), active = rd32(0x4ac4ec), pc = x86_poll_pc;
	watchdog(&s, now, frame, active, pc);
	if (!warning[0])
		last_key[0] = 0; /* a cause that clears and comes back is logged again */

	const struct present_stats *ps = &s.ps;
	char voice[64], vq[160], wq[600], started[32];
	int music = sound_status(voice, sizeof voice);
	json_str(vq, sizeof vq, voice);
	json_str(wq, sizeof wq, warning);
	strftime(started, sizeof started, "%Y-%m-%dT%H:%M:%SZ", gmtime(&start_wall));
	uint32_t black = ps->last_nonblack ? now - ps->last_nonblack : now - start_ticks;
	static char buf[8192];
	snprintf(buf, sizeof buf,
		"{\n"
		"  \"pid\": %d, \"started\": \"%s\", \"uptime_ms\": %u, \"stage\": \"running\", \"init_ms\": %u,\n"
		"  \"mode\": {\"headless\": %d, \"scripted\": %d, \"env\": %s},\n"
		"  \"desktop\": {\"locked\": %d, \"asleep\": %d, \"app_hidden\": %d},\n"
		"  \"window\": {\"count\": %d, \"os_windows\": %d, \"id\": %u, \"flags\": %u, \"shown\": %d, "
		"\"hidden\": %d, \"size\": [%d, %d], \"pos\": [%d, %d], \"drawable\": [%d, %d], \"focus\": %d, "
		"\"occluded\": %d},\n"
		"  \"present\": {\"renderer\": \"%s\", \"frames_in\": %lu, \"presented\": %lu, "
		"\"skipped\": {\"occluded\": %lu, \"no_drawable\": %lu, \"size0\": %lu}, "
		"\"last_wait_ms\": %.1f, \"worst_wait_ms\": %.1f, \"since_present_ms\": %d, "
		"\"luma_in\": %d, \"luma_shown\": %d, \"black_ms\": %u, "
		"\"probe\": {\"count\": %lu, \"luma\": %d, \"luma_src\": %d, \"age_ms\": %d, \"nonblack_age_ms\": %d}},\n"
		"  \"game\": {\"frame\": %u, \"frame_still_ms\": %u, \"active_4ac4ec\": %u, \"timer\": %d, "
		"\"poll_pc\": \"%08x\", \"mips\": %.1f, \"main_age_ms\": %u},\n"
		"  \"logo\": %d, \"music\": %d, \"voice\": \"%s\",\n"
		"  \"warning\": \"%s\", \"warning_key\": \"%s\", \"warnings\": %u, \"warned\": %s\n"
		"}\n",
		(int)getpid(), started, now - launch_ticks, start_ticks - launch_ticks, headless, scripted, env_json,
		locked, asleep, ps->app_hidden, s.windows, ps->os_windows,
		window ? SDL_GetWindowID(window) : 0, s.flags, !!(s.flags & SDL_WINDOW_SHOWN),
		!!(s.flags & SDL_WINDOW_HIDDEN), s.w, s.h, s.x, s.y, ps->drawable_w, ps->drawable_h, s.focus,
		ps->occluded, present_name(), ps->frames_in, ps->presented, ps->skip_occluded, ps->skip_drawable,
		ps->skip_size, ps->last_wait_ms, ps->worst_wait_ms,
		ps->last_present ? (int)(now - ps->last_present) : -1, ps->luma_in, ps->luma_shown, black,
		ps->probes, ps->luma_probe, ps->luma_probe_src, ps->last_probe ? (int)(now - ps->last_probe) : -1,
		ps->last_probe_nonblack ? (int)(now - ps->last_probe_nonblack) : -1, frame,
		now - last_frame_change, active, win32_timer_running(), pc, mips, now - s.tick,
		SDL_AtomicGet(&logo), music, vq, wq, warning_key, warnings, warned);
	write_json(buf);
}

static int writer(void *ud)
{
	while (!SDL_AtomicGet(&quit)) {
		write_status();
		for (int i = 0; i < 10 && !SDL_AtomicGet(&quit); i++)
			SDL_Delay(100);
	}
	return 0;
}

static void on_signal(int sig)
{
#ifdef _WIN32
	remove(path);
#else
	unlink(path); /* async-signal-safe */
#endif
	signal(sig, SIG_DFL);
	raise(sig);
}

/* status files of instances that died without removing theirs (SIGKILL) */
static void remove_stale(const char *dir)
{
#ifndef _WIN32
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		int pid;
		char rest[16];
		if (sscanf(e->d_name, "nanpa2-status-%d.%15s", &pid, rest) != 2 || pid == (int)getpid())
			continue;
		if (kill(pid, 0) < 0 && errno == ESRCH) {
			char p[1400];
			snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
			unlink(p);
		}
	}
	closedir(d);
#endif
}

void status_main(void)
{
	static uint32_t last;
	static int first = 1;
	uint32_t now = SDL_GetTicks();
	if (!lock || (!first && now - last < 250))
		return;
	first = 0;
	last = now;
	struct snap s = { 0 };
	s.tick = now ? now : 1;
	for (Uint32 id = 1; id < 64; id++)
		s.windows += SDL_GetWindowFromID(id) != NULL;
	if (window) {
		s.flags = SDL_GetWindowFlags(window);
		SDL_GetWindowSize(window, &s.w, &s.h);
		SDL_GetWindowPosition(window, &s.x, &s.y);
		s.focus = (s.flags & SDL_WINDOW_INPUT_FOCUS) != 0;
	}
	present_stats(&s.ps);
	SDL_LockMutex(lock);
	snap = s;
	SDL_UnlockMutex(lock);
}

void status_set_logo(int playing)
{
	SDL_AtomicSet(&logo, playing);
}

void status_early(void)
{
	if (thread)
		return;
	start_wall = time(NULL);
	launch_ticks = SDL_GetTicks();
	headless = getenv("NANPA2_HEADLESS") != NULL;
	collect_env();
	/* TMPDIR (macOS, Linux), TEMP (Windows) */
	const char *dir = getenv("TMPDIR");
	if (!dir || !*dir)
		dir = getenv("TEMP");
	if (!dir || !*dir)
		dir = getenv("TMP");
	if (!dir || !*dir)
		dir = "/tmp";
	size_t n = strlen(dir);
	snprintf(path, sizeof path, "%s%snanpa2-status-%d.json", dir,
		 n && (dir[n - 1] == '/' || dir[n - 1] == '\\') ? "" : "/", (int)getpid());
	snprintf(tmp_path, sizeof tmp_path, "%s.tmp", path);
	remove_stale(dir);
	lock = SDL_CreateMutex();
	atexit(status_shutdown);
	/* SIGINT and SIGTERM stay with SDL (SDL_QUIT: a clean exit, which
	 * removes the file); SIGALRM is the perl alarm wrapper of test runs */
#ifndef _WIN32
	signal(SIGHUP, on_signal);
	signal(SIGALRM, on_signal);
#endif
	thread = SDL_CreateThread(writer, "status", NULL);
	SDL_Log("status: %s", path);
}

void status_stage(const char *name)
{
	stage = name;
}

void status_init(SDL_Window *win, double mips_setting)
{
	status_early();
	window = win;
	mips = mips_setting;
	start_ticks = SDL_GetTicks();
	last_frame_change = start_ticks;
	scripted = dev_scripted();
	status_main();
	stage = "running";
	SDL_AtomicSet(&phase, 1);
}

void status_shutdown(void)
{
	if (!thread)
		return;
	SDL_AtomicSet(&quit, 1);
	SDL_WaitThread(thread, NULL);
	thread = NULL;
	remove(path);
	remove(tmp_path);
}
