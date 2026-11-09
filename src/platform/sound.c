/* Music and voices, replacing the native DirectSound code and the MP3
 * decoder linked into nanpa2.exe.
 *
 * The native engine has exactly two DirectSound secondary buffers:
 *
 * - music (buffer 0x4be560, file image 0x4be570, flag 0x4be54c = 1 for WAV).
 *   0x402cb0 stops the old music (0x4032c0), opens MUSIC\%02d.MP3, else
 *   MUSIC\%02d.WAV. MP3: 0x4030f0 makes a 0x24000-byte ring buffer at
 *   44100 Hz 16-bit stereo, refilled half by half (0x4034a0 waits on the
 *   two notification events 0x4be550/0x4be554, 0x4031d0 decodes; at the end
 *   of the stream it rewinds the decoder, 0x4084b0(dec, 0)), and plays it
 *   with DSBPLAY_LOOPING: MP3 music loops forever. WAV: 0x403020 copies the
 *   data chunk (0x402f10) into a static buffer, Play(0, 0, 0): it plays
 *   once. The short MUSIC\4x-5x.WAV jingles go through this channel too,
 *   so a jingle replaces the BGM; there is no separate effects channel.
 * - voice (buffer 0x4be564, file image 0x4be574). 0x403360 stops the old
 *   voice (0x403440), loads SPEECH%s into a static buffer, rewinds and plays
 *   it once. The game's window procedure stops the voice on WM_LBUTTONDOWN,
 *   WM_RBUTTONDOWN, the key-up of Return/Space/Esc and on deactivation.
 *
 * No buffer is ever given a volume, pan or frequency, so everything plays at
 * full level, mixed additively with clipping like the DirectSound mixer.
 * (0x402ad0 sets the primary buffer to 22050 Hz 8-bit stereo; this is a
 * Win9x output format hint and is not emulated.)
 *
 * Dev hook: NANPA2_AUDIO_DUMP=out.wav writes the mixed output (44100 Hz
 * S16 stereo) to a WAV file. Without an audio device the SDL dummy driver
 * runs the mixer in real time, so the dump and game timing are the same. */
#include "platform/sound.h"

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"

#include "hooks.h"
#include "fsys.h"

#define RATE 44100

static SDL_AudioDeviceID dev;
static int log_on;

/* PCM clip: 16-bit interleaved at its own rate/channels */
struct clip {
	int16_t *pcm;
	size_t frames;
	int channels, rate;
	double pos; /* in source frames */
	int active;
};

static struct {
	mp3dec_ex_t mp3;
	unsigned char *file;
	int is_mp3, loaded, active, paused;
	int num;
	struct clip wav;
	int16_t buf[4096];
	size_t buf_len, buf_pos;
	int mp3_rate, mp3_ch;
	double mp3_frac;
	int16_t mp3_prev[2], mp3_cur[2];
} music;

static struct clip voice;

static struct {
	FILE *f;
	uint32_t frames, patched;
} dump;

/* ---- WAV dump ---- */

static void put32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

static void dump_header(void)
{
	unsigned char h[44];
	uint32_t bytes = dump.frames * 4u;
	memcpy(h, "RIFF", 4);
	put32(h + 4, 36 + bytes);
	memcpy(h + 8, "WAVEfmt ", 8);
	put32(h + 16, 16);
	put32(h + 20, 1 | 2u << 16); /* PCM, stereo */
	put32(h + 24, RATE);
	put32(h + 28, RATE * 4);
	put32(h + 32, 4 | 16u << 16); /* block align 4, 16 bits */
	memcpy(h + 36, "data", 4);
	put32(h + 40, bytes);
	long at = ftell(dump.f);
	fseek(dump.f, 0, SEEK_SET);
	fwrite(h, 1, sizeof h, dump.f);
	fseek(dump.f, at < 44 ? 44 : at, SEEK_SET);
	fflush(dump.f);
}

static void dump_open(void)
{
	const char *path = getenv("NANPA2_AUDIO_DUMP");
	if (!path || !*path || dump.f)
		return;
	dump.f = fopen(path, "wb");
	if (!dump.f) {
		SDL_Log("audio dump: cannot write %s", path);
		return;
	}
	dump.frames = dump.patched = 0;
	dump_header();
}

static void dump_write(const int16_t *s, int frames)
{
	if (!dump.f)
		return;
	fwrite(s, 4, (size_t)frames, dump.f);
	dump.frames += (uint32_t)frames;
	/* runs may be killed (alarm), so keep the header valid every second */
	if (dump.frames - dump.patched >= RATE) {
		dump.patched = dump.frames;
		dump_header();
	}
}

static void dump_close(void)
{
	if (!dump.f)
		return;
	dump_header();
	fclose(dump.f);
	dump.f = NULL;
}

static void slog(const char *what, const char *name)
{
	if (log_on)
		fprintf(stderr, "sound %s %s at %u ms (dump %.3f s)\n", what, name, hook_now(),
			(double)dump.frames / RATE);
}

/* ---- decoding and mixing ---- */

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

/* Mix a clip once into out (stereo at RATE, linear interpolation).
 * Returns 0 when it reached its end. */
static int mix_clip(struct clip *c, int32_t *out, int frames)
{
	double step = (double)c->rate / RATE;
	int ch = c->channels;
	for (int i = 0; i < frames; i++) {
		size_t f = (size_t)c->pos;
		if (f >= c->frames)
			return 0;
		size_t g = f + 1 < c->frames ? f + 1 : f;
		double t = c->pos - (double)f;
		const int16_t *a = c->pcm + f * (size_t)ch, *b = c->pcm + g * (size_t)ch;
		int32_t l = (int32_t)(a[0] + (b[0] - a[0]) * t);
		int32_t r = ch > 1 ? (int32_t)(a[1] + (b[1] - a[1]) * t) : l;
		out[2 * i] += l;
		out[2 * i + 1] += r;
		c->pos += step;
	}
	return 1;
}

/* next decoded MP3 frame; at the end rewind, as 0x4031d0 does on -0x202 */
static int mp3_next(int16_t lr[2])
{
	if (music.buf_pos >= music.buf_len) {
		size_t want = sizeof music.buf / sizeof music.buf[0];
		size_t got = mp3dec_ex_read(&music.mp3, music.buf, want);
		if (!got) {
			mp3dec_ex_seek(&music.mp3, 0);
			got = mp3dec_ex_read(&music.mp3, music.buf, want);
			if (!got)
				return 0;
		}
		music.buf_len = got;
		music.buf_pos = 0;
	}
	lr[0] = music.buf[music.buf_pos];
	lr[1] = music.mp3_ch > 1 ? music.buf[music.buf_pos + 1] : lr[0];
	music.buf_pos += (size_t)music.mp3_ch;
	return 1;
}

static void mix_mp3(int32_t *out, int frames)
{
	/* the disc's MP3s are all 44100 Hz stereo; resample anything else */
	double step = (double)music.mp3_rate / RATE;
	for (int i = 0; i < frames; i++) {
		while (music.mp3_frac >= 1.0) {
			music.mp3_prev[0] = music.mp3_cur[0];
			music.mp3_prev[1] = music.mp3_cur[1];
			if (!mp3_next(music.mp3_cur)) {
				music.active = 0;
				return;
			}
			music.mp3_frac -= 1.0;
		}
		double t = music.mp3_frac;
		out[2 * i] += (int32_t)(music.mp3_prev[0] + (music.mp3_cur[0] - music.mp3_prev[0]) * t);
		out[2 * i + 1] += (int32_t)(music.mp3_prev[1] + (music.mp3_cur[1] - music.mp3_prev[1]) * t);
		music.mp3_frac += step;
	}
}

static void callback(void *ud, Uint8 *stream, int len)
{
	(void)ud;
	int frames = len / 4;
	int32_t mix[1024 * 2];
	int16_t *o = (int16_t *)stream;
	while (frames > 0) {
		int n = frames > 1024 ? 1024 : frames;
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
		dump_write(o, n);
		o += 2 * n;
		frames -= n;
	}
}

/* ---- native entry points ---- */

static SDL_AudioDeviceID open_device(void)
{
	SDL_AudioSpec want = { 0 }, have;
	want.freq = RATE;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	want.callback = callback;
	return SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
}

/* native 0x402ad0. The caller (0x402620) ignores the result: without sound
 * the game runs silently (natively after a MessageBox warning). */
int sound_init(void)
{
	if (dev)
		return 1;
	log_on = getenv("NANPA2_LOG") != NULL;
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
		dev = open_device();
	if (!dev) {
		SDL_Log("audio: no device (%s)%s", SDL_GetError(),
			getenv("NANPA2_AUDIO_DUMP") ? ", using the dummy driver for the dump" : "");
		if (!getenv("NANPA2_AUDIO_DUMP"))
			return 0;
		/* the dump still needs a real-time mixer */
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
		if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
			dev = open_device();
		if (!dev) {
			SDL_Log("audio: dummy driver failed (%s)", SDL_GetError());
			return 0;
		}
	}
	if (log_on)
		fprintf(stderr, "audio: driver %s\n", SDL_GetCurrentAudioDriver());
	dump_open();
	SDL_PauseAudioDevice(dev, 0);
	return 1;
}

/* native 0x402c70 (game exit, 0x402a20) */
void sound_shutdown(void)
{
	if (!dev)
		return;
	sound_voice_stop();
	sound_music_stop();
	SDL_CloseAudioDevice(dev); /* joins the callback thread */
	dev = 0;
	dump_close();
}

static char voice_name[64];

static void music_free(void)
{
	if (music.is_mp3)
		mp3dec_ex_close(&music.mp3);
	free(music.file);
	free(music.wav.pcm);
	music.file = NULL;
	music.wav.pcm = NULL;
	music.active = music.loaded = music.is_mp3 = 0;
}

/* native 0x4032c0 */
void sound_music_stop(void)
{
	if (!dev)
		return;
	SDL_LockAudioDevice(dev);
	if (music.loaded)
		slog("music stop", "");
	music_free();
	SDL_UnlockAudioDevice(dev);
}

/* native 0x402cb0 */
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
		music.mp3_rate = music.mp3.info.hz > 0 ? music.mp3.info.hz : RATE;
		music.mp3_ch = music.mp3.info.channels > 0 ? music.mp3.info.channels : 2;
		music.mp3_frac = 1.0;
		memset(music.mp3_cur, 0, sizeof music.mp3_cur);
		music.loaded = music.active = 1;
	} else {
		free(d);
		snprintf(rel, sizeof rel, "MUSIC\\%02d.WAV", n);
		d = fsys_read(rel, &len);
		if (d && load_wav(d, len, &music.wav))
			music.loaded = music.active = 1; /* Play(0, 0, 0): once */
		free(d);
	}
	music.num = n;
	music.paused = 0;
	SDL_UnlockAudioDevice(dev);
	if (music.active)
		slog("music", rel);
	else
		SDL_Log("music %02d not found", n);
}

/* native 0x403220 (deactivated: Stop if playing) and 0x403250 (activated:
 * Play again, looping for MP3, once for WAV). A stopped DirectSound buffer
 * keeps its position, and a one-shot buffer that played to its end is back
 * at 0, so a finished WAV piece starts over on reactivation. */
void sound_music_pause(int pause)
{
	if (!dev)
		return;
	SDL_LockAudioDevice(dev);
	if (music.loaded) {
		if (!pause && !music.is_mp3 && !music.active) {
			music.wav.pos = 0;
			music.active = 1;
		}
		music.paused = pause;
	}
	SDL_UnlockAudioDevice(dev);
}

/* For the status file: the music number playing (-1 none) and the last
 * voice started, if it still plays. Read without the audio lock: a stale
 * value is harmless there. */
int sound_status(char *voice_out, size_t cap)
{
	snprintf(voice_out, cap, "%s", dev && voice.active ? voice_name : "");
	return dev && music.active && !music.paused ? music.num : -1;
}

/* native 0x403440 */
void sound_voice_stop(void)
{
	if (!dev)
		return;
	SDL_LockAudioDevice(dev);
	if (voice.active && log_on)
		slog("voice cut", "");
	voice.active = 0;
	free(voice.pcm);
	voice.pcm = NULL;
	SDL_UnlockAudioDevice(dev);
}

/* native 0x403360 */
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
		voice.active = 1;
		SDL_UnlockAudioDevice(dev);
		snprintf(voice_name, sizeof voice_name, "%s", rel);
		slog("voice", rel);
	} else {
		SDL_Log("voice %s not found", rel);
	}
	free(d);
}

/* Offline check (nanpa2 --wav-check FILE...): decode through load_wav, the
 * path of voices and WAV jingles, and compare the duration with the data
 * chunk length / byte rate from the header, walking the RIFF chunks
 * independently. Returns the number of failures. */
static uint32_t le32(const unsigned char *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

int sound_wav_check(int n, char **files)
{
	int bad = 0;
	for (int i = 0; i < n; i++) {
		FILE *f = fopen(files[i], "rb");
		if (!f) {
			printf("%s: cannot open\n", files[i]);
			bad++;
			continue;
		}
		fseek(f, 0, SEEK_END);
		size_t len = (size_t)ftell(f);
		fseek(f, 0, SEEK_SET);
		unsigned char *d = malloc(len + 1);
		if (fread(d, 1, len, f) != len)
			len = 0;
		fclose(f);
		uint32_t rate = 0, brate = 0, dlen = 0;
		unsigned ch = 0, bits = 0;
		for (size_t p = 12; len >= 12 && p + 8 <= len;) {
			uint32_t sz = le32(d + p + 4);
			if (!memcmp(d + p, "fmt ", 4) && p + 24 <= len) {
				ch = d[p + 10] | d[p + 11] << 8;
				rate = le32(d + p + 12);
				brate = le32(d + p + 16);
				bits = d[p + 22] | d[p + 23] << 8;
			} else if (!memcmp(d + p, "data", 4)) {
				dlen = sz;
				if (p + 8 + (size_t)dlen > len)
					dlen = (uint32_t)(len - p - 8);
				break;
			}
			p += 8 + (size_t)sz + (sz & 1);
		}
		struct clip c = { 0 };
		int ok = brate && load_wav(d, len, &c);
		double want = brate ? (double)dlen / brate : 0;
		double got = ok ? (double)c.frames / c.rate : -1;
		/* mixer output: source frames consumed at rate/RATE per output frame */
		double mixed = ok ? ceil((double)c.frames / ((double)c.rate / RATE)) / RATE : -1;
		int pass = ok && c.channels == (int)ch && (uint32_t)c.rate == rate && fabs(got - want) < 1e-4 &&
			   fabs(mixed - want) < 1e-3;
		printf("%s: %s ch %u rate %u bits %u | decoded ch %d rate %d frames %zu = %.4f s, mixed %.4f s, "
		       "expected %.4f s\n",
		       files[i], pass ? "ok" : "FAIL", ch, rate, bits, c.channels, c.rate, c.frames, got, mixed,
		       want);
		bad += !pass;
		free(c.pcm);
		free(d);
	}
	return bad;
}
