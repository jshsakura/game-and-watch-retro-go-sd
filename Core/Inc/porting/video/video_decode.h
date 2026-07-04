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
#include "avi.h"

// Frame slots carved out of g_scratch: the player reads AVI video chunks ahead
// into free slots during the pacing wait (jitter buffer), then decodes each
// slot in display order. See video_decode.c for the g_scratch partition.
#define VIDEO_FRAME_MAX (64 * 1024)
#define VIDEO_SLOTS     3

uint8_t *video_slot(int i);

void video_decode_init(void);
void video_decode_deinit(void);
// HW-decode a frame that was already read into a slot buffer.
bool video_decode_slot(const uint8_t *src, long size, uint16_t *fb, int fb_w, int fb_h);
