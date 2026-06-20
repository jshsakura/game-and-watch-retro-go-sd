// Video app entry — a /video browser that plays MJPEG-AVI clips. Shares the
// Music overlay (only one homebrew runs at a time), reusing tjpgd + the overlay
// infra. Launched from the homebrew list as "Video".
#pragma once

#include <stdint.h>

void app_main_video(uint8_t load_state, uint8_t start_paused, int8_t save_slot);
