/* Stage 5B: file access in C (src/game/file.c) */
#ifndef NANPA2_GAME_FILE_H
#define NANPA2_GAME_FILE_H

#include <stdint.h>

uint32_t game_file_open(uint32_t name);                   /* 0x401000, EAX = stream */
void game_file_load(uint32_t name, uint32_t buf);         /* 0x401050 */
void game_file_save(uint32_t name, uint32_t buf, uint32_t size); /* 0x401160 */
void game_file_open_regs(void);
void game_file_load_regs(void);
void game_file_save_regs(void);

#endif
