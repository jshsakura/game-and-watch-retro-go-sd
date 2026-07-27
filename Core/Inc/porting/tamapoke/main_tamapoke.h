/* Entry point, called by the launcher's homebrew dispatch.
 *
 * Homebrew is matched by name in resident launcher code, so this core cannot
 * be added to an existing firmware by copying its .bin to the card: the
 * launcher branch and the overlay must come from the same build.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void app_main_tamapoke(uint8_t load_state, uint8_t start_paused, int8_t save_slot);

#ifdef __cplusplus
}
#endif
