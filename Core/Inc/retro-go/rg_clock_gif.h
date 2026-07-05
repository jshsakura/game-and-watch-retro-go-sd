#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Animated GIF background for the Clock app (/clock/bg.gif), sized for a full
 * 320x240. Frames are decoded ONE at a time on their delay (a full-screen GIF
 * is too big to cache every frame), then scale-filled into the LCD. Transient
 * decoder + one frame buffer in the launcher heap while no emulator runs;
 * freed on unload, so an emulator's RAM is never reduced. */

bool clock_gif_load(void);                       /* open + alloc; false if none/too big */
void clock_gif_free(void);
bool clock_gif_ready(void);
void clock_gif_blit(uint16_t *fb, uint32_t now); /* decode-on-delay + scale-fill blit */
