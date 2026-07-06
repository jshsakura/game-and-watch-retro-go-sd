#ifndef STUB_FAV_ODROID_SYSTEM_H
#define STUB_FAV_ODROID_SYSTEM_H
/* Minimal stand-in for the host favorites test: rg_favorites.c only needs
 * RG_PATH_MAX (normally pulled in transitively via config.h) and rg_calloc
 * (used once, in rg_favorites_register_tab). */
#include <stdbool.h>
#include <stddef.h>

#ifndef RG_PATH_MAX
#define RG_PATH_MAX 255
#endif

void *rg_calloc(size_t nmemb, size_t size);

#endif
