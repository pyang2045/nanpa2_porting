/* System fonts through CoreText (font_apple.h). */
#include "platform/font_apple.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>
#include <SDL3/SDL.h>
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Family names in order of preference. The Ming/Song faces come first: they are the closest to
 * MingLiU; PingFang TC is on every iOS. Songti TC and LiSong Pro are downloadable system fonts on
 * iOS (Settings > General > Fonts), fetched below. */
static const char *const tier_names[2][8] = {
	{ "Songti TC", "LiSong Pro", "Apple LiSung", "STSongti-TC-Regular", "PingFang TC", NULL },
	{ "Songti TC", "PingFang TC", NULL },
};

static CFStringRef cfs(const char *s)
{
	return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

static CTFontDescriptorRef match_family(const char *name)
{
	CFStringRef n = cfs(name);
	const void *keys[1] = { kCTFontFamilyNameAttribute };
	const void *vals[1] = { n };
	CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
						   &kCFTypeDictionaryValueCallBacks);
	CTFontDescriptorRef d = CTFontDescriptorCreateWithAttributes(attrs);
	CFSetRef must = CFSetCreate(NULL, (const void **)&kCTFontFamilyNameAttribute, 1, &kCFTypeSetCallBacks);
	CTFontDescriptorRef hit = NULL;
	CFArrayRef all = CTFontDescriptorCreateMatchingFontDescriptors(d, must);
	/* the regular face of the family, else the first */
	if (all && CFArrayGetCount(all) > 0) {
		CFIndex best = 0, bestscore = 1 << 30;
		for (CFIndex i = 0; i < CFArrayGetCount(all); i++) {
			CTFontDescriptorRef c = (CTFontDescriptorRef)CFArrayGetValueAtIndex(all, i);
			CFStringRef ps = CTFontDescriptorCopyAttribute(c, kCTFontNameAttribute);
			int score = 100;
			if (ps) {
				char b[128] = "";
				CFStringGetCString(ps, b, sizeof b, kCFStringEncodingUTF8);
				if (strstr(b, "Regular"))
					score = 0;
				else if (strstr(b, "Medium"))
					score = 10;
				else if (strstr(b, "Light"))
					score = 20;
				CFRelease(ps);
			}
			if (score < bestscore) {
				bestscore = score;
				best = i;
			}
		}
		hit = (CTFontDescriptorRef)CFArrayGetValueAtIndex(all, best);
		CFRetain(hit);
	}
	if (all)
		CFRelease(all);
	CFRelease(must);
	CFRelease(d);
	CFRelease(attrs);
	CFRelease(n);
	return hit;
}

#if TARGET_OS_IPHONE
/* Ask the system to download the family (needs the network the first time); wait a bounded time. */
static void download_family(const char *name, double seconds)
{
	CFStringRef n = cfs(name);
	const void *keys[1] = { kCTFontFamilyNameAttribute };
	const void *vals[1] = { n };
	CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
						   &kCFTypeDictionaryValueCallBacks);
	CTFontDescriptorRef d = CTFontDescriptorCreateWithAttributes(attrs);
	CFArrayRef descs = CFArrayCreate(NULL, (const void **)&d, 1, &kCFTypeArrayCallBacks);
	CFSetRef must = CFSetCreate(NULL, (const void **)&kCTFontFamilyNameAttribute, 1, &kCFTypeSetCallBacks);
	dispatch_semaphore_t sem = dispatch_semaphore_create(0);
	SDL_Log("Font: requesting %s from the system", name);
	bool started = CTFontDescriptorMatchFontDescriptorsWithProgressHandler(
		descs, must, ^bool(CTFontDescriptorMatchingState state, CFDictionaryRef progress) {
			(void)progress;
			if (state == kCTFontDescriptorMatchingDidFinish ||
			    state == kCTFontDescriptorMatchingDidFailWithError)
				dispatch_semaphore_signal(sem);
			return true;
		});
	if (started && dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * 1e9))))
		SDL_Log("Font: %s download not finished in %.0f s (continues in the background)", name, seconds);
	CFRelease(must);
	CFRelease(descs);
	CFRelease(d);
	CFRelease(attrs);
	CFRelease(n);
}
#endif

static void *read_file(CFURLRef url, size_t *size)
{
	char path[1024];
	if (!url || !CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, sizeof path))
		return NULL;
	return SDL_LoadFile(path, size);
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

struct tab {
	uint32_t tag, len;
	CFDataRef data;
};

static int tab_cmp(const void *a, const void *b)
{
	uint32_t x = ((const struct tab *)a)->tag, y = ((const struct tab *)b)->tag;
	return x < y ? -1 : x > y;
}

/* A single sfnt from CTFontCopyTable: header, table directory with checksums, the tables 4-aligned. */
static void *rebuild_sfnt(CTFontRef font, size_t *size)
{
	CFArrayRef tags = CTFontCopyAvailableTables(font, kCTFontTableOptionNoOptions);
	if (!tags)
		return NULL;
	CFIndex nt = CFArrayGetCount(tags);
	struct tab *t = calloc((size_t)(nt ? nt : 1), sizeof *t);
	size_t total = 12 + 16 * (size_t)nt;
	int n = 0;
	for (CFIndex i = 0; i < nt; i++) {
		uintptr_t tag = (uintptr_t)CFArrayGetValueAtIndex(tags, i);
		CFDataRef d = CTFontCopyTable(font, (CTFontTableTag)tag, kCTFontTableOptionNoOptions);
		if (!d)
			continue;
		t[n].tag = (uint32_t)tag;
		t[n].len = (uint32_t)CFDataGetLength(d);
		t[n].data = d;
		total += ((size_t)t[n].len + 3) & ~(size_t)3;
		n++;
	}
	CFRelease(tags);
	qsort(t, (size_t)n, sizeof *t, tab_cmp);
	uint8_t *out = SDL_calloc(1, total);
	if (!out || !n) {
		SDL_free(out);
		for (int i = 0; i < n; i++)
			CFRelease(t[i].data);
		free(t);
		return NULL;
	}
	int pow2 = 1, lg = 0;
	while (pow2 * 2 <= n) {
		pow2 *= 2;
		lg++;
	}
	put32(out, 0x00010000);
	out[4] = (uint8_t)(n >> 8);
	out[5] = (uint8_t)n;
	out[6] = (uint8_t)((pow2 * 16) >> 8);
	out[7] = (uint8_t)(pow2 * 16);
	out[8] = (uint8_t)(lg >> 8);
	out[9] = (uint8_t)lg;
	out[10] = (uint8_t)(((n - pow2) * 16) >> 8);
	out[11] = (uint8_t)((n - pow2) * 16);
	size_t off = 12 + 16 * (size_t)n;
	for (int i = 0; i < n; i++) {
		uint8_t *e = out + 12 + 16 * i;
		const uint8_t *src = CFDataGetBytePtr(t[i].data);
		uint32_t sum = 0;
		for (uint32_t k = 0; k < t[i].len; k += 4) {
			uint32_t w = 0;
			for (uint32_t b = 0; b < 4; b++)
				w = w << 8 | (k + b < t[i].len ? src[k + b] : 0);
			sum += w;
		}
		put32(e, t[i].tag);
		put32(e + 4, sum);
		put32(e + 8, (uint32_t)off);
		put32(e + 12, t[i].len);
		memcpy(out + off, src, t[i].len);
		off += ((size_t)t[i].len + 3) & ~(size_t)3;
		CFRelease(t[i].data);
	}
	free(t);
	*size = total;
	return out;
}

static void *face_bytes(CTFontDescriptorRef d, size_t *size, int *from_file)
{
	CTFontRef font = CTFontCreateWithFontDescriptor(d, 16, NULL);
	if (!font)
		return NULL;
	const char *mode = getenv("NANPA2_TEXT"); /* "tables": rebuild even when the file opens (test) */
	CFURLRef url = CTFontCopyAttribute(font, kCTFontURLAttribute);
	void *data = mode && strstr(mode, "tables") ? NULL : read_file(url, size);
	*from_file = data != NULL;
	if (url)
		CFRelease(url);
	if (!data)
		data = rebuild_sfnt(font, size);
	CFRelease(font);
	return data;
}

const char *font_apple_name(int tier, int index)
{
	return tier_names[tier & 1][index];
}

int font_apple_count(int tier)
{
	int n = 0;
	while (tier_names[tier & 1][n])
		n++;
	return n;
}

void *font_apple_load(int tier, int index, size_t *size, char *label, size_t label_size)
{
	static int requested;
	const char *name = tier_names[tier & 1][index];
	CTFontDescriptorRef d = match_family(name);
#if TARGET_OS_IPHONE
	if (!d && tier == 0 && index == 0 && !requested) {
		/* Songti TC is a downloadable font on iOS: ask for it once a run, waiting a bounded time */
		requested = 1;
		download_family(name, 5.0);
		d = match_family(name);
	}
#else
	(void)requested;
#endif
	if (!d)
		return NULL;
	int from_file;
	void *data = face_bytes(d, size, &from_file);
	CFRelease(d);
	if (data) {
		snprintf(label, label_size, "CoreText:%s", name);
		SDL_Log("Font: %s read %s (%zu bytes)", label, from_file ? "from its file" : "rebuilt from its tables",
			*size);
	}
	return data;
}
