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

/* --- SD-file favorites (zero resident RAM) ------------------------------
 * One favorite per line in /favorites.txt: the full ROM path
 * ("/roms/<system>/<file>"). The file is read only on discrete UI events
 * (A-menu open, favorites-tab entry) — NEVER in the list-render hot path;
 * per-frame file IO in the render loop is what killed the 2026-06 SD-DB
 * attempt. The favorites tab (tab 0) materializes its list into the same
 * shared 1000-slot buffer every emulator tab already reuses, so the whole
 * feature costs no RAM. Implemented in Core/Src/retro-go/rg_favorites.c. */

/** True if path is favorited (one full read of /favorites.txt). */
bool rg_favorites_contains(const char *path);
/** Append path (no-op if already present). */
bool rg_favorites_add(const char *path);
/** Rewrite the file without path (temp file + rename, never in-place). */
bool rg_favorites_remove(const char *path);
/** Delete all favorites. */
bool rg_favorites_reset(void);
/** Register the ★ tab; MUST be the first tab added (tab index 0). */
void rg_favorites_register_tab(void);
/** True when the launcher is currently showing the favorites tab. */
bool rg_favorites_is_current_tab(void);

/* Bridges into rg_emulators.c internals the favorites tab borrows. */
/** The shared per-tab ROM list buffer (NULL before emulators_init). */
retro_emulator_file_t *rg_emulators_shared_file_buffer(int *maxcount);
/** System descriptor for a /roms/<dirname> system, or NULL. */
const rom_system_t *rg_emulators_system_for_dir(const char *dirname, size_t len);
