#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "rg_emulators.h"

/* Game-list sort mode, kept in the parent repo (not the retro-go-stm32
 * submodule) so the feature builds without bumping the submodule pin. The
 * persistence backing (sort mode) lives in the fixed persistent_config_t
 * and is defined in odroid_settings.c. */

/** Game list sort modes (cycled by the user in the game overlay). */
typedef enum
{
    ODROID_SORT_NAME = 0,   // alphabetical
    ODROID_SORT_ADDED,      // file/scan order (as added to the SD card)
    ODROID_SORT_COUNT
} odroid_sort_mode_t;

/** Persisted game list sort mode (odroid_sort_mode_t). */
uint8_t odroid_settings_SortMode_get(void);
void odroid_settings_SortMode_set(uint8_t mode);
