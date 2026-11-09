/*
 * Platform API: music and voices (R4, docs/refactor.md "The platform API").
 *
 * The game asks for music by number and for voice clips by the script's name; the platform
 * decodes the files (MP3 or WAV under MUSIC\ and SPEECH\) and plays them on its own audio
 * thread. Nothing here returns state into the game, so replays are unaffected.
 */
#ifndef NANPA2_PLATFORM_SOUND_H
#define NANPA2_PLATFORM_SOUND_H

#include <stddef.h>

int sound_init(void);
void sound_shutdown(void);
void sound_music_play(int n);
void sound_music_stop(void);
/* music number playing or -1; the voice file name in voice_out */
int sound_status(char *voice_out, size_t cap);
void sound_music_pause(int pause);
/* rel: the file under the game folder, "SPEECH" + the script's name */
void sound_voice_play(const char *rel);
void sound_voice_stop(void);
/* --wav-check; returns failures */
int sound_wav_check(int n, char **files);

#endif
