#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Animated GIF background for the Clock app. Lowest-load model: decode every
 * frame ONCE on load into an RGB565 cache, then playback is a pure blit at the
 * GIF's own frame delay — no per-frame LZW decode. The cache is transient
 * (freed on unload) and lives in the launcher heap while no emulator runs, so
 * it never reduces an emulator's memory. GIF is /clock/bg.gif. */

bool clock_gif_load(void);                       /* decode+cache; false if none/too big */
void clock_gif_free(void);
bool clock_gif_ready(void);
void clock_gif_blit(uint16_t *fb, uint32_t now); /* advance by delay + scale-fill blit */
