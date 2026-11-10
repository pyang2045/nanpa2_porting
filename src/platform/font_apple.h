/*
 * Apple system fonts through CoreText (font_apple.c), for text.c. iOS apps cannot read the system
 * font files, so there is no path to give stb_truetype; the face comes out of CoreText instead,
 * as the font file when that opens and otherwise as an sfnt rebuilt from its tables. macOS uses
 * this after its font file paths. Internal to src/platform/; elsewhere there is no such header.
 */
#ifndef NANPA2_PLATFORM_FONT_APPLE_H
#define NANPA2_PLATFORM_FONT_APPLE_H

#include <stddef.h>

/* Tier 0 is the primary face (Songti TC, LiSong Pro, Apple LiSung, STSongti-TC: Ming/Song serif
 * Traditional Chinese; then PingFang TC), tier 1 the fallback for kana and the full-width tilde
 * (a face with kana: Songti TC, then PingFang TC; it must cover Traditional ideographs too, as text.c probes). font_apple_count: the names of the tier. font_apple_load: the
 * index'th, as a buffer from SDL_malloc (the caller owns it; a font file or a single sfnt), its
 * size, and a log label; NULL when that family is not on this system. On iOS the first call of tier
 * 0 may wait a few seconds for the downloadable Songti TC. */
int font_apple_count(int tier);
const char *font_apple_name(int tier, int index); /* the family name; the label is "CoreText:" + it */
void *font_apple_load(int tier, int index, size_t *size, char *label, size_t label_size);

#endif
