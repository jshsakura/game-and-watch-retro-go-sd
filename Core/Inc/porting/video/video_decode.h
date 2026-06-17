// One-frame JPEG decode for the Video player: reads `size` bytes of a baseline
// JPEG from the file's current position and decodes (via tjpgd) straight into
// the RGB565 framebuffer, scaled to fit WxH and centered. Returns false if the
// frame is not decodable (caller keeps the previous frame on screen).
#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

bool video_decode_frame(FILE *f, long size, uint16_t *fb, int fb_w, int fb_h);
