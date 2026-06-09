#pragma once

#include <stdint.h>

// Homebrew "Media" app entry point.
// Dispatched from rg_emulators.c when the launcher selects /roms/homebrew/Media.bin.
void app_main_media(uint8_t load_state, uint8_t start_paused, int8_t save_slot);
