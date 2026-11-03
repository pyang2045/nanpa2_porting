#ifndef NANPA2_DEV_H
#define NANPA2_DEV_H

#include <stdint.h>

void dev_init(void);
uint32_t dev_now(void);
void dev_frame(const uint8_t *frame, const uint32_t pal[256]);
void dev_inject(void);

#endif
