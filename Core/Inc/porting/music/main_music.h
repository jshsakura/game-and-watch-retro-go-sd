#pragma once

#include <stdint.h>

// Homebrew "Music" app entry point.
// Dispatched from rg_emulators.c when the launcher selects /roms/homebrew/Music.bin.
void app_main_music(uint8_t load_state, uint8_t start_paused, int8_t save_slot);
