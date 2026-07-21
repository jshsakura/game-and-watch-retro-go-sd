#pragma once
#include <stdint.h>

/* CPS-1 (Capcom arcade) core entry point. See
 * Core/Src/porting/cps1/main_cps1.c for the memory plan and the device-only
 * behaviours it encodes. */
void app_main_cps1(uint8_t load_state, uint8_t start_paused, int8_t save_slot);
