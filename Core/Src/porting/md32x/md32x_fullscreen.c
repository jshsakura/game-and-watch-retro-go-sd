/* See md32x_fullscreen.h for what this is for and why it works in place.
 *
 * The whole subtlety is the direction of the copy. Source row for output row y
 * is s(y) = top + floor(y * lines / 240), and for the 224-line case that is
 * 8 + floor(14y/15):
 *
 *   y <  120 -> s(y) > y   the row is being pulled UP, so a top-down sweep is
 *                          safe: every row it reads is one it has not written.
 *   y == 120 -> s(y) = y   fixed point, nothing to do.
 *   y >  120 -> s(y) < y   the row is being pushed DOWN, so a bottom-up sweep
 *                          is safe for the mirror-image reason.
 *
 * Doing it in one direction would overwrite source rows before reading them.
 * The crossover is derived rather than hardcoded, so a different content rect
 * (a 30-row-mode game reports 240 lines and is skipped; a PAL frame reports
 * something else again) stays correct.
 *
 * Its own dependency-free TU, like md32x_border_clear.c and for the same
 * reason: tests/test_md32x_fullscreen.c compiles THIS file rather than
 * reimplementing it. A harness that is a different program proves nothing.
 */
#include "md32x_fullscreen.h"

#include <string.h>

#define ROW_BYTES ((size_t)MD32X_FS_WIDTH * sizeof(uint16_t))

void md32x_fullscreen_expand(uint16_t *fb, int top, int lines)
{
    if (fb == NULL) return;
    if (lines <= 0 || lines >= MD32X_FS_HEIGHT) return;
    if (top < 0 || top + lines > MD32X_FS_HEIGHT) return;

    /* The fixed point: the one output row that reads itself. Rows above it
     * move up, rows below it move down. Solved rather than assumed, so this
     * holds for any (top, lines) that passed the guards above. */
    int fixed = MD32X_FS_HEIGHT;
    for (int y = 0; y < MD32X_FS_HEIGHT; y++) {
        if (top + (int)(((long)y * lines) / MD32X_FS_HEIGHT) <= y) { fixed = y; break; }
    }

    for (int y = 0; y < fixed; y++) {
        int src = top + (int)(((long)y * lines) / MD32X_FS_HEIGHT);
        memcpy(fb + (size_t)y * MD32X_FS_WIDTH,
               fb + (size_t)src * MD32X_FS_WIDTH, ROW_BYTES);
    }
    for (int y = MD32X_FS_HEIGHT - 1; y >= fixed; y--) {
        int src = top + (int)(((long)y * lines) / MD32X_FS_HEIGHT);
        if (src == y) continue;
        memcpy(fb + (size_t)y * MD32X_FS_WIDTH,
               fb + (size_t)src * MD32X_FS_WIDTH, ROW_BYTES);
    }
}
