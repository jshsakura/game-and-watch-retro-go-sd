#include <string.h>

#include "favorites.h"
#include "odroid_settings.h"
#include "crc32.h"

/* A favorite is identified by the crc32 of its full file path. This is
 * cheap (no ROM data read) and stable across reboots. The hash is never
 * allowed to be 0, since 0 marks an empty slot in the config store. */
static uint32_t favorite_hash(const retro_emulator_file_t *file)
{
    if (file == NULL || file->path[0] == '\0')
        return 0;

    uint32_t h = crc32_le(0, (const unsigned char *)file->path, strlen(file->path));
    return (h == 0) ? 1u : h;
}

bool favorite_is(const retro_emulator_file_t *file)
{
    return odroid_settings_favorite_has(favorite_hash(file));
}

bool favorite_toggle(retro_emulator_file_t *file)
{
    uint32_t h = favorite_hash(file);
    if (h == 0)
        return false;

    bool now;
    if (odroid_settings_favorite_has(h)) {
        odroid_settings_favorite_remove(h);
        now = false;
    } else {
        now = odroid_settings_favorite_add(h);
    }

    odroid_settings_commit();
    return now;
}
