/* Music and voices, replacing the native DirectSound streaming and the MP3
 * decoder linked into nanpa2.exe (0x402cb0 play music, 0x403360 play voice).
 * MUSIC\NN.MP3 loops (native 0x4031d0 seeks back to 0 at the end);
 * MUSIC\NN.WAV and voices play once. */
#include "hle.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"

#include "fsys.h"

#define RATE 44100

static SDL_AudioDeviceID dev;

/* PCM clip: 16-bit interleaved at its own rate/channels */
struct clip {
	int16_t *pcm;
	size_t frames;
	int channels, rate;
	double pos; /* in source frames */
	int loop;
	int active;
};

static struct {
	mp3dec_ex_t mp3;
	unsigned char *file;
	int is_mp3, active, paused;
	struct clip wav;
	int16_t buf[4096];
	size_t buf_len, buf_pos;
} music;

static struct clip voice;

static int load_wav(const unsigned char *d, size_t n, struct clip *c)
{
	SDL_AudioSpec spec;
	Uint8 *buf;
	Uint32 len;
	SDL_RWops *rw = SDL_RWFromConstMem(d, (int)n);
	if (!SDL_LoadWAV_RW(rw, 1, &spec, &buf, &len))
		return 0;
	SDL_AudioCVT cvt;
	SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_S16SYS, spec.channels,
			  spec.freq);
	cvt.len = (int)len;
	cvt.buf = malloc((size_t)len * (size_t)(cvt.len_mult > 0 ? cvt.len_mult : 1) + 16);
	memcpy(cvt.buf, buf, len);
	SDL_FreeWAV(buf);
	if (cvt.needed)
		SDL_ConvertAudio(&cvt);
	int out_len = cvt.needed ? cvt.len_cvt : cvt.len;
	c->pcm = (int16_t *)cvt.buf;
	c->channels = spec.channels;
	c->rate = spec.freq;
	c->frames = (size_t)out_len / (2u * (unsigned)spec.channels);
	c->pos = 0;
	return 1;
}

/* Mix a clip into out (stereo S16 at RATE). Returns 0 when it ended. */
static int mix_clip(struct clip *c, int32_t *out, int frames)
{
	double step = (double)c->rate / RATE;
	for (int i = 0; i < frames; i++) {
		size_t f = (size_t)c->pos;
		if (f >= c->frames) {
			if (!c->loop)
				return 0;
			c->pos = 0;
			f = 0;
		}
		int16_t l = c->pcm[f * (size_t)c->channels];
		int16_t r = c->channels > 1 ? c->pcm[f * (size_t)c->channels + 1] : l;
		out[2 * i] += l;
		out[2 * i + 1] += r;
		c->pos += step;
	}
	return 1;
}

static void mix_mp3(int32_t *out, int frames)
{
	int ch = music.mp3.info.channels;
	for (int i = 0; i < frames; i++) {
		if (music.buf_pos >= music.buf_len) {
			size_t got = mp3dec_ex_read(&music.mp3, music.buf, sizeof music.buf / sizeof music.buf[0]);
			if (!got) {
				mp3dec_ex_seek(&music.mp3, 0); /* loop */
				got = mp3dec_ex_read(&music.mp3, music.buf, sizeof music.buf / sizeof music.buf[0]);
				if (!got) {
					music.active = 0;
					return;
				}
			}
			music.buf_len = got;
			music.buf_pos = 0;
		}
		int16_t l = music.buf[music.buf_pos];
		int16_t r = ch > 1 ? music.buf[music.buf_pos + 1] : l;
		music.buf_pos += (size_t)ch;
		out[2 * i] += l;
		out[2 * i + 1] += r;
	}
}

static void callback(void *ud, Uint8 *stream, int len)
{
	int frames = len / 4;
	int32_t mix[4096 * 2];
	int16_t *o = (int16_t *)stream;
	while (frames > 0) {
		int n = frames > 4096 ? 4096 : frames;
		memset(mix, 0, sizeof(int32_t) * (size_t)n * 2);
		if (music.active && !music.paused) {
			if (music.is_mp3)
				mix_mp3(mix, n);
			else if (!mix_clip(&music.wav, mix, n))
				music.active = 0;
		}
		if (voice.active && !mix_clip(&voice, mix, n))
			voice.active = 0;
		for (int i = 0; i < 2 * n; i++) {
			int32_t v = mix[i];
			o[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
		o += 2 * n;
		frames -= n;
	}
}

int sound_init(void)
{
	if (dev)
		return 1;
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
		return 0;
	SDL_AudioSpec want = { 0 }, have;
	want.freq = RATE;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	want.callback = callback;
	dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (!dev)
		return 0;
	SDL_PauseAudioDevice(dev, 0);
	return 1;
}

static void music_free(void)
{
	if (music.is_mp3)
		mp3dec_ex_close(&music.mp3);
	free(music.file);
	free(music.wav.pcm);
	music.file = NULL;
	music.wav.pcm = NULL;
	music.active = 0;
	music.is_mp3 = 0;
}

void sound_music_stop(void)
{
	if (!dev)
		return;
	SDL_LockAudioDevice(dev);
	music_free();
	SDL_UnlockAudioDevice(dev);
}

void sound_music_play(int n)
{
	if (!dev)
		return;
	char rel[64];
	size_t len;
	sound_music_stop();
	snprintf(rel, sizeof rel, "MUSIC\\%02d.MP3", n);
	unsigned char *d = fsys_read(rel, &len);
	SDL_LockAudioDevice(dev);
	if (d && !mp3dec_ex_open_buf(&music.mp3, d, len, MP3D_SEEK_TO_SAMPLE)) {
		music.file = d;
		music.is_mp3 = 1;
		music.buf_len = music.buf_pos = 0;
		music.active = 1;
	} else {
		free(d);
		snprintf(rel, sizeof rel, "MUSIC\\%02d.WAV", n);
		d = fsys_read(rel, &len);
		if (d && load_wav(d, len, &music.wav)) {
			music.wav.loop = 0;
			music.active = 1;
		}
		free(d);
	}
	music.paused = 0;
	SDL_UnlockAudioDevice(dev);
	if (!music.active)
		SDL_Log("music %02d not found", n);
}

void sound_music_pause(int pause)
{
	music.paused = pause;
}

void sound_voice_stop(void)
{
	if (!dev)
		return;
	SDL_LockAudioDevice(dev);
	voice.active = 0;
	free(voice.pcm);
	voice.pcm = NULL;
	SDL_UnlockAudioDevice(dev);
}

void sound_voice_play(const char *rel)
{
	if (!dev)
		return;
	size_t len;
	sound_voice_stop();
	unsigned char *d = fsys_read(rel, &len);
	struct clip c = { 0 };
	if (d && load_wav(d, len, &c)) {
		SDL_LockAudioDevice(dev);
		voice = c;
		voice.loop = 0;
		voice.active = 1;
		SDL_UnlockAudioDevice(dev);
	} else {
		SDL_Log("voice %s not found", rel);
	}
	free(d);
}
