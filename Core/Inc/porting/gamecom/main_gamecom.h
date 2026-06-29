#ifndef MAIN_GAMECOM_H
#define MAIN_GAMECOM_H

#include <stdint.h>

/* Tiger Game.com (Sharp SM8500/SM8521) emulator entry point. */
void app_main_gamecom(uint8_t load_state, uint8_t start_paused, int8_t save_slot);

#endif /* MAIN_GAMECOM_H */
