/* Host-test stub for Core/Inc/gw_malloc.h.
 *
 * rg_favorites.c allocates favorites_emu with ahb_calloc (it used to be
 * rg_calloc, until upstream 8caa3e45 dropped -DDEBUG_RG_ALLOC and the firmware
 * moved these launcher-lifetime structs into the AHB pool). Unlike the
 * common_stubs placeholder, this one has to actually provide the function --
 * the test links rg_favorites.c for real and would otherwise fail to resolve it.
 *
 * calloc() is the honest host equivalent: on device ahb_calloc returns
 * zero-initialised memory from the AHB bump pool, and the only property this
 * test depends on is "zeroed block of the requested size". The pool's real
 * behaviour (invalidation by ahb_init() before a core launch) has no analogue
 * on the host and is not what these tests exercise. */
#ifndef STUB_GW_MALLOC_H
#define STUB_GW_MALLOC_H

#include <stdlib.h>

static inline void *ahb_calloc(size_t count, size_t size)
{
    return calloc(count, size);
}

#endif
