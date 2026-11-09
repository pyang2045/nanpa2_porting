#include "assets.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fsys.h"
#include "launch.h"
#include "x86/x86.h"

#define ASSET_NAME "nanpa2.dat"
#define EXE_NAME "nanpa2.exe"

/* the modes that run the original code need the exe (the oracle) */
static const char *exe_mode(void)
{
	static const char *const names[] = { "NANPA2_INTERP" };
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
		const char *e = getenv(names[i]);
		if (e && *e && strcmp(e, "0"))
			return names[i];
	}
	return NULL;
}

static int exists(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f)
		fclose(f);
	return f != NULL;
}

/* the candidate places for the asset: the game folder, then the save folder */
static int asset_path(int which, char *out, size_t cap)
{
	if (which == 0) {
		snprintf(out, cap, "%s/" ASSET_NAME, fsys_root());
		return 1;
	}
	if (which > 1 || !launch_pref_dir())
		return 0;
	snprintf(out, cap, "%s/" ASSET_NAME, launch_pref_dir());
	return 1;
}

/* the header of an asset of this build (the full check is x86_asset_read's) */
static int header_ok(const char *path)
{
	uint8_t h[16];
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	size_t n = fread(h, 1, sizeof h, f);
	fclose(f);
	return n == sizeof h && !memcmp(h, "N2AS", 4) && h[4] == 1 && !h[5] && !h[6] && !h[7] && h[8] == 0x6a &&
	       h[9] == 0x7c && h[10] == 0x69 && h[11] == 0x39 && h[12] == 0 && h[13] == 0x10 && h[14] == 0x0c &&
	       h[15] == 0;
}

static int exe_ok(const char *path)
{
	uint8_t head[4096];
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	size_t n = fread(head, 1, sizeof head, f);
	fclose(f);
	return x86_exe_check(head, n);
}

const char *assets_check(const char *dir)
{
	(void)dir;
	char p[1200];
	for (int i = 0; asset_path(i, p, sizeof p); i++)
		if (header_ok(p))
			return NULL;
	if (!fsys_path(EXE_NAME, p, sizeof p))
		return "no nanpa2.dat and no nanpa2.exe. nanpa2.exe is inside data1.cab on the CD (an "
		       "InstallShield cabinet): extract nanpa2.exe and FLAG0..FLAG6 from it into this "
		       "folder (or copy them from an installed copy of the game). It is needed once, to "
		       "make nanpa2.dat";
	if (!exe_ok(p))
		return "nanpa2.exe is not the Taiwan release this engine runs "
		       "(同級生２, built 2000-07-10)";
	return NULL;
}

/* write the asset to the first of the game folder and the save folder that
 * takes it; path receives where */
static int make_somewhere(const char *exe, char *path, size_t cap, int *rc)
{
	for (int i = 0; asset_path(i, path, cap); i++) {
		*rc = x86_asset_make(exe, path);
		if (*rc == 1 || *rc < 0)
			return *rc == 1;
	}
	return 0;
}

int assets_open(const char *exe_arg, char *err, size_t cap)
{
	char exe[1200], p[1200];
	int have_exe;
	if (exe_arg) {
		snprintf(exe, sizeof exe, "%s", exe_arg);
		have_exe = exists(exe);
	} else
		have_exe = fsys_path(EXE_NAME, exe, sizeof exe);
	const char *why = exe_arg ? "--exe" : exe_mode();
	if (why) {
		if (!have_exe) {
			snprintf(err, cap, "%s needs the original code, so nanpa2.exe: %s not found (it is "
				 "inside data1.cab on the CD)", why, exe);
			return 0;
		}
		int st = x86_load_image(exe);
		if (st <= 0) {
			snprintf(err, cap, st < 0 ? "%s is not the nanpa2.exe this engine runs (the Taiwan "
					 "release built 2000-07-10)" : "cannot load %s", exe);
			return 0;
		}
		SDL_Log("assets: %s loaded whole (%s)", exe, why);
		return 1;
	}
	int stale = 0;
	for (int i = 0; asset_path(i, p, sizeof p); i++) {
		if (!exists(p))
			continue;
		int st = x86_asset_read(p);
		if (st == 1) {
			SDL_Log("assets: %s", p);
			return 1;
		}
		SDL_Log("assets: %s is not usable (%s)", p, st < 0 ? "damaged or from another build" : "cannot read");
		stale = 1;
	}
	if (!have_exe) {
		snprintf(err, cap, stale ? "nanpa2.dat is damaged or from another build, and there is no nanpa2.exe "
					   "in %s to make a new one from"
					 : "no nanpa2.dat and no nanpa2.exe in %s. nanpa2.exe (inside data1.cab on "
					   "the CD) is needed once, to make nanpa2.dat",
			 fsys_root());
		return 0;
	}
	int rc = 0;
	if (!make_somewhere(exe, p, sizeof p, &rc)) {
		if (rc < 0) {
			snprintf(err, cap, "%s is not the nanpa2.exe this engine runs (the Taiwan release built "
					   "2000-07-10)", exe);
			return 0;
		}
		SDL_Log("assets: cannot write " ASSET_NAME " to the game folder or the save folder, "
			"loading %s whole", exe);
		int st = x86_load_image(exe);
		if (st <= 0) {
			snprintf(err, cap, "cannot load %s", exe);
			return 0;
		}
		return 1;
	}
	SDL_Log("assets: made %s from %s", p, exe);
	if (x86_asset_read(p) != 1) {
		snprintf(err, cap, "cannot load %s just made", p);
		return 0;
	}
	return 1;
}

int assets_make(const char *exe_arg, const char *out, char *path, size_t pathcap, char *err, size_t cap)
{
	char exe[1200];
	if (exe_arg)
		snprintf(exe, sizeof exe, "%s", exe_arg);
	else if (!fsys_path(EXE_NAME, exe, sizeof exe)) {
		snprintf(err, cap, "no nanpa2.exe in %s (it is inside data1.cab on the CD)", fsys_root());
		return 0;
	}
	int rc = 0;
	if (out) {
		snprintf(path, pathcap, "%s", out);
		rc = x86_asset_make(exe, path);
	} else
		make_somewhere(exe, path, pathcap, &rc);
	if (rc == 1)
		return 1;
	snprintf(err, cap, rc < 0 ? "%s is not the nanpa2.exe this engine runs (the Taiwan release built "
				    "2000-07-10)" : "cannot read %s or write %s", exe, path);
	return 0;
}
