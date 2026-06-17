// One-frame JPEG decode for the Video player. Uses the STM32 hardware JPEG codec
// (decode -> YCbCr -> DMA2D -> RGB565 straight into the framebuffer), centered as
// a letterbox. Call video_decode_init() once before playback and
// video_decode_deinit() after. Returns false if the frame is not decodable or is
// larger than the screen (the HW codec cannot downscale) — the caller keeps the
// previous frame on screen.
#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

void video_decode_init(void);
void video_decode_deinit(void);
bool video_decode_frame(FILE *f, long size, uint16_t *fb, int fb_w, int fb_h);
