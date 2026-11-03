/* LOGO.AVI, native 0x401e20.
 *
 * The native code switches DirectDraw to 640x480x16, opens the file with
 * MCI ("open avivideo!%s alias vfw", retried with the CD prefix), points it
 * at the game window ("window vfw handle %d", "put vfw destination at 0 0
 * 640 480"), plays it with notify and pumps messages through a temporary
 * window procedure (0x401fd0) until 0x4ac4f8 is cleared: by MM_MCINOTIFY
 * (0x3b9) with MCI_NOTIFY_SUCCESSFUL, or by WM_KEYDOWN, WM_LBUTTONDOWN or
 * WM_RBUTTONDOWN (skip). WM_ACTIVATEAPP pauses/resumes the movie.
 *
 * Here the AVI is parsed directly: video is Cinepak (src/cinepak.c), shown
 * at the stream rate through present_frame_rgb; audio is PCM, queued on an
 * SDL audio device of its own (the game's sound device is opened later, by
 * 0x402ad0). Scripted runs (NANPA2_INPUT or NANPA2_SHOT_DIR) skip it so
 * their timings stay valid; NANPA2_LOGO=0/1 forces it off/on. */
#include "logo.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>

#include "cinepak.h"
#include "fsys.h"
#include "platform/present.h"
#include "status.h"

struct avi {
	unsigned char *data;
	size_t len;
	int w, h;
	uint32_t usec_per_frame;
	int nframes;
	const uint8_t **frame;
	uint32_t *frame_len;
	uint8_t *audio;
	size_t audio_len;
	int a_rate, a_channels, a_bits;
};

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static unsigned le16(const uint8_t *p)
{
	return (unsigned)p[0] | (unsigned)p[1] << 8;
}

/* Walk RIFF chunks in [p, end); lists are entered. */
static void walk(struct avi *a, const uint8_t *p, const uint8_t *end, int *stream)
{
	while (p + 8 <= end) {
		uint32_t sz = le32(p + 4);
		const uint8_t *body = p + 8, *next = body + sz + (sz & 1);
		if (body + sz > end)
			break;
		if (!memcmp(p, "LIST", 4) || !memcmp(p, "RIFF", 4)) {
			if (sz >= 4)
				walk(a, body + 4, body + sz, stream);
		} else if (!memcmp(p, "avih", 4) && sz >= 40) {
			a->usec_per_frame = le32(body);
		} else if (!memcmp(p, "strh", 4) && sz >= 8) {
			*stream = !memcmp(body, "vids", 4) ? (memcmp(body + 4, "cvid", 4) ? -1 : 0)
				: !memcmp(body, "auds", 4) ? 1 : -1;
		} else if (!memcmp(p, "strf", 4)) {
			if (*stream == 0 && sz >= 12) {
				a->w = (int)le32(body + 4);
				a->h = (int)le32(body + 8);
			} else if (*stream == 1 && sz >= 16 && le16(body) == 1) { /* WAVE_FORMAT_PCM */
				a->a_channels = (int)le16(body + 2);
				a->a_rate = (int)le32(body + 4);
				a->a_bits = (int)le16(body + 14);
			}
		} else if (!memcmp(p + 2, "dc", 2) || !memcmp(p + 2, "db", 2)) {
			if (!memcmp(p, "00", 2)) {
				a->frame = realloc(a->frame, sizeof *a->frame * (size_t)(a->nframes + 1));
				a->frame_len = realloc(a->frame_len, sizeof *a->frame_len * (size_t)(a->nframes + 1));
				a->frame[a->nframes] = body;
				a->frame_len[a->nframes++] = sz;
			}
		} else if (!memcmp(p, "01wb", 4)) {
			a->audio = realloc(a->audio, a->audio_len + sz);
			memcpy(a->audio + a->audio_len, body, sz);
			a->audio_len += sz;
		}
		p = next;
	}
}

static void avi_free(struct avi *a)
{
	free(a->data);
	free(a->frame);
	free(a->frame_len);
	free(a->audio);
	memset(a, 0, sizeof *a);
}

static int avi_open(struct avi *a)
{
	memset(a, 0, sizeof *a);
	a->data = fsys_read("LOGO.AVI", &a->len);
	if (!a->data)
		return 0;
	int stream = -1;
	walk(a, a->data, a->data + a->len, &stream);
	if (a->w != SCREEN_W || a->h != SCREEN_H || !a->nframes) {
		SDL_Log("LOGO.AVI: unsupported (%dx%d, %d cvid frames)", a->w, a->h, a->nframes);
		avi_free(a);
		return 0;
	}
	if (!a->usec_per_frame)
		a->usec_per_frame = 33333;
	return 1;
}

static struct cvid dec;

int logo_frame(int n, uint32_t *xrgb)
{
	struct avi a;
	if (!avi_open(&a))
		return 0;
	int count = a.nframes;
	if (n >= count || !cvid_init(&dec, a.w, a.h)) {
		avi_free(&a);
		return 0;
	}
	for (int i = 0; i <= n; i++)
		cvid_decode(&dec, a.frame[i], a.frame_len[i]);
	memcpy(xrgb, dec.pixels, (size_t)SCREEN_W * SCREEN_H * 4);
	cvid_free(&dec);
	avi_free(&a);
	return count;
}

static SDL_AudioDeviceID open_audio(const struct avi *a)
{
	if (!a->audio_len || !a->a_rate || (a->a_bits != 8 && a->a_bits != 16) || a->a_channels < 1 ||
	    a->a_channels > 2)
		return 0;
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
		return 0;
	SDL_AudioSpec want = { 0 };
	want.freq = a->a_rate;
	want.format = a->a_bits == 8 ? AUDIO_U8 : AUDIO_S16LSB;
	want.channels = (Uint8)a->a_channels;
	want.samples = 1024;
	SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
	if (!dev) {
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return 0;
	}
	SDL_QueueAudio(dev, a->audio, (Uint32)a->audio_len);
	return dev;
}

static int wanted(void)
{
	const char *e = getenv("NANPA2_LOGO");
	if (e)
		return atoi(e) != 0;
	return !getenv("NANPA2_INPUT") && !getenv("NANPA2_SHOT_DIR");
}

void logo_play(void)
{
	struct avi a;
	if (!wanted() || !avi_open(&a))
		return;
	if (!cvid_init(&dec, a.w, a.h)) {
		avi_free(&a);
		return;
	}
	status_set_logo(1);
	int cursor = SDL_ShowCursor(SDL_QUERY);
	SDL_ShowCursor(SDL_DISABLE); /* native ShowCursor(0) */
	SDL_AudioDeviceID dev = open_audio(&a);
	if (dev)
		SDL_PauseAudioDevice(dev, 0);

	Uint64 start = SDL_GetPerformanceCounter(), freq = SDL_GetPerformanceFrequency();
	int done = 0, shown = 0, i;
	for (i = 0; i < a.nframes && !done; i++) {
		cvid_decode(&dec, a.frame[i], a.frame_len[i]);
		Uint64 due = start + freq * a.usec_per_frame * (Uint64)i / 1000000u;
		Uint64 now = SDL_GetPerformanceCounter();
		/* late by more than a frame: decode only, catch up */
		if (now > due + freq * a.usec_per_frame / 1000000u && i + 1 < a.nframes)
			continue;
		while ((now = SDL_GetPerformanceCounter()) < due) {
			Uint32 ms = (Uint32)((due - now) * 1000 / freq);
			SDL_Delay(ms > 1 ? ms - 1 : 0);
		}
		present_frame_rgb(dec.pixels);
		shown++;
		status_main();
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_QUIT) {
				SDL_PushEvent(&ev); /* for the game's own loop */
				done = 1;
				break;
			}
			if ((ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) && present_hotkey(&ev.key))
				continue;
			if ((ev.type == SDL_KEYDOWN && !ev.key.repeat) || ev.type == SDL_MOUSEBUTTONDOWN)
				done = 1; /* WM_KEYDOWN, WM_LBUTTONDOWN, WM_RBUTTONDOWN: skip */
		}
	}
	/* hold the last frame for its duration */
	if (!done)
		SDL_Delay(a.usec_per_frame / 1000);
	SDL_Log("LOGO.AVI: %s after %d of %d frames (%d shown) in %u ms", done ? "skipped" : "ended", i,
		a.nframes, shown, (unsigned)((SDL_GetPerformanceCounter() - start) * 1000 / freq));
	if (dev) {
		SDL_CloseAudioDevice(dev);
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	}
	SDL_ShowCursor(cursor);
	status_set_logo(0);
	cvid_free(&dec);
	avi_free(&a);
}
