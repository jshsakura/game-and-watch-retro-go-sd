// On-device JPEG-frame decode benchmark — sizes the video player's
// resolution/fps budget. See video_bench.c.
#pragma once

// Decode a bundled 320x240 baseline-JPEG frame N times, show ms/frame + fps on
// the LCD, and wait for a key. Call once at app start.
void video_decode_bench(void);
