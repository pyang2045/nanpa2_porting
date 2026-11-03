#ifndef NANPA2_DEV_H
#define NANPA2_DEV_H

#include <stdint.h>

void dev_init(void);
uint32_t dev_now(void);
void dev_frame(const uint8_t *frame, const uint32_t pal[256]);
void dev_inject(void);
void dev_file_opened(const char *name);
void dev_vm_fetch(uint32_t off);
/* NANPA2_INPUT given: the host mouse, keyboard and focus are ignored */
int dev_scripted(void);

#endif
