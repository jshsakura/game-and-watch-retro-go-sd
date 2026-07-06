/* Shared host-test stubs: watchdog no-op + the two path helpers that
 * rg_storage.c calls (verbatim copies of rg_utils.c's implementations). */
#include "odroid_system.h"
#include <string.h>

void wdog_refresh(void) {}

const char *rg_basename(const char *path)
{
    if (!path)
        return ".";

    const char *name = strrchr(path, '/');
    return name ? name + 1 : path;
}

const char *rg_extension(const char *path)
{
    if (!path)
        return NULL;

    const char *ptr = rg_basename(path);
    const char *ext = strrchr(ptr, '.');
    if (!ext)
        return ptr + strlen(ptr);
    return ext + 1;
}
