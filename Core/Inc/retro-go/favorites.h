#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "rg_emulators.h"

/* Favorites + game-list sort, kept in the parent repo (not the retro-go-stm32
 * submodule) so the feature builds without bumping the submodule pin. The
 * persistence backing (path-hash store + sort mode) lives in the fixed
 * persistent_config_t and is defined in odroid_settings.c. */

/** Game list sort modes (cycled by the user in the game overlay). */
typedef enum
{
    ODROID_SORT_NAME = 0,   // alphabetical
    ODROID_SORT_ADDED,      // file/scan order (as added to the SD card)
    ODROID_SORT_FAVORITES,  // favorites first, then alphabetical
    ODROID_SORT_COUNT
} odroid_sort_mode_t;

/** Persisted game list sort mode (odroid_sort_mode_t). */
uint8_t odroid_settings_SortMode_get(void);
void odroid_settings_SortMode_set(uint8_t mode);

/** Favorite membership by path hash (0 = invalid). Caller must
 * odroid_settings_commit() after add/remove to persist. */
bool odroid_settings_favorite_has(uint32_t hash);
bool odroid_settings_favorite_add(uint32_t hash);
bool odroid_settings_favorite_remove(uint32_t hash);

/** True if this ROM file is currently marked as a favorite. */
bool favorite_is(const retro_emulator_file_t *file);

/** Toggle the favorite state of a ROM file and persist it.
 *  Returns the new state (true = now a favorite). */
bool favorite_toggle(retro_emulator_file_t *file);
