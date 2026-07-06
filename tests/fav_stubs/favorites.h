#ifndef STUB_FAV_FAVORITES_H
#define STUB_FAV_FAVORITES_H
/* Host stand-in for Core/Inc/retro-go/favorites.h: same public surface,
 * without pulling the real rg_emulators.h (would double-define types
 * already provided by tests/fav_stubs/rg_emulators.h). */
#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    ODROID_SORT_NAME = 0,
    ODROID_SORT_ADDED,
    ODROID_SORT_COUNT
} odroid_sort_mode_t;

uint8_t odroid_settings_SortMode_get(void);
void odroid_settings_SortMode_set(uint8_t mode);

bool rg_favorites_contains(const char *path);
bool rg_favorites_add(const char *path);
bool rg_favorites_remove(const char *path);
bool rg_favorites_reset(void);
void rg_favorites_register_tab(void);
bool rg_favorites_is_current_tab(void);

#endif
