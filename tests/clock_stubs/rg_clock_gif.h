#ifndef STUB_RG_CLOCK_GIF_H
#define STUB_RG_CLOCK_GIF_H
#include <stdint.h>
#include <stdbool.h>
bool clock_gif_ready(void);
void clock_gif_blit(uint16_t *fb, uint32_t now);
void clock_gif_load(void);
void clock_gif_free(void);
#endif
