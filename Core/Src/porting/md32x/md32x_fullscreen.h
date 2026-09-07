/* Fill the 8-pixel bars a V28 (224-line) 32X frame leaves at the top and
 * bottom of the 240-line panel, by expanding the finished frame vertically in
 * place.
 *
 * Why in place, and why here rather than a generic scaler: every other core
 * that scales renders into its own framebuffer and blits through one. 32X has
 * picodrive paint straight into the LCD active buffer
 * (PicoDrawSetOutBuf(lcd_get_active_buffer(), 320*2)), deliberately, because a
 * 320x240x2 intermediate is 150 KB and this overlay has hundreds of bytes
 * spare, not hundreds of kilobytes. So the expansion has to happen inside the
 * buffer that already holds the frame, with no second buffer anywhere.
 *
 * The cost is row moves, not pixel work: 240 rows x 640 bytes = 150 KB per
 * drawn frame, which at this core's frame times is under 1%.
 *
 * Call AFTER picodrive has painted the frame and BEFORE lcd_swap().
 */
#ifndef MD32X_FULLSCREEN_H
#define MD32X_FULLSCREEN_H

#include <stdint.h>

#define MD32X_FS_WIDTH  320
#define MD32X_FS_HEIGHT 240

/* Expand `lines` rows starting at row `top` to cover all MD32X_FS_HEIGHT rows
 * of `fb`, in place. Nearest-neighbour: output row y is a copy of input row
 * floor(y * lines / MD32X_FS_HEIGHT), so for the usual 224 -> 240 case one row
 * in every 14 is duplicated.
 *
 * Does nothing when there is nothing to do or the rect is not sane: a frame
 * that already fills the panel (lines >= 240), an empty one (lines <= 0), a
 * negative top, or a rect that runs off the bottom.
 */
void md32x_fullscreen_expand(uint16_t *fb, int top, int lines);

#endif /* MD32X_FULLSCREEN_H */
