#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Photo-album background for the clock (launcher context).
 *
 * While the clock runs it BORROWS the launcher's `shared_files` ROM-list buffer
 * (~500K) as a photo arena — the full-screen clock doesn't need the game list.
 * It scans /clock/album for RAW RGB565 photos (exactly 320x240x2 bytes) and
 * shows one at a time. On exit the caller (rg_clock) rebuilds the launcher's
 * lists via rg_emulators_reset_all_lists() + a current-tab refresh, so nothing
 * is corrupted — no reboot, faster than one tab switch.
 *
 * Phase 2 = raw .565 only (isolates the borrow/exit from HW-JPEG risk). JPEG is
 * a later phase. Everything is defensive: any failure -> not ready -> the clock
 * falls back to a solid background; NEVER asserts on the launcher's memory. */

bool            clock_album_open(void);       /* borrow + scan + load first; false = none/no room */
bool            clock_album_ready(void);       /* a photo is loaded and blittable */
const uint16_t *clock_album_current(void);     /* 320x240 RGB565 buffer, or NULL */
void            clock_album_advance(void);      /* hard-swap to the next photo (dissolve = later) */
int             clock_album_count(void);        /* number of photos found */
void            clock_album_close(void);        /* forget state (arena was borrowed in place) */
