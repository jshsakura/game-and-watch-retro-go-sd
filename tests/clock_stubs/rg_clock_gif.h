#ifndef STUB_RG_CLOCK_GIF_H
#define STUB_RG_CLOCK_GIF_H
#include <stdint.h>
#include <stdbool.h>
bool clock_gif_ready(void);
void clock_gif_blit(uint16_t *fb, uint32_t now);
#define CLOCK_GIF_OK 0
#define CLOCK_GIF_NO_FILE 1
#define CLOCK_GIF_NO_RAM 2
#define CLOCK_GIF_BAD_DIMS 3
int clock_gif_status(void);
bool clock_gif_load(void);
void clock_gif_free(void);
#endif
