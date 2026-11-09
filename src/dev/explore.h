#ifndef NANPA2_EXPLORE_H
#define NANPA2_EXPLORE_H

#include <stdint.h>

/* NANPA2_EXPLORE=seed: the exploration bot (explore.c), driven from dev.c */
void explore_init(void);
int explore_on(void);
void explore_file(const char *name);
void explore_vm(uint32_t off);
void explore_hits(uint32_t table);
void explore_tick(void);

#endif
