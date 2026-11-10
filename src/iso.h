#ifndef NANPA2_ISO_H
#define NANPA2_ISO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Read-only ISO 9660 image (the game CD as a .iso; re-notes.md "ISO image"). Host file
 * access like fsys: the game never sees it. Names are matched case-insensitively with "/"
 * or "\" separators, ";1" stripped. The Joliet tree is used when the image has one. */

/* Open the image and index every directory. 0 if it is not a usable ISO 9660 image
 * (cooked 2048-byte sectors only). Mounting the path that is already mounted does nothing. */
int iso_mount(const char *path);
void iso_unmount(void);
int iso_mounted(void);
const char *iso_image_path(void);

/* A file's extent (byte offset in the image, length); 1 if rel names a file. */
int iso_stat(const char *rel, int64_t *off, uint32_t *size);

/* The whole file in a malloc'd buffer (NULL if missing). */
unsigned char *iso_read(const char *rel, size_t *len);

/* A read-only FILE over the file's bytes (seekable). POSIX: a cookie stream reading the
 * extent on demand; Windows: a temporary file. NULL if missing. */
FILE *iso_fopen(const char *rel);

/* Names of the files directly in directory dir ("" = root), original case. */
void iso_list(const char *dir, void (*cb)(const char *name, void *arg), void *arg);

#endif
