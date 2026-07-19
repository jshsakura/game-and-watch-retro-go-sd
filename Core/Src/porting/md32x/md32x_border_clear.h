/* Fixes the border-row flicker after closing the in-game overlay while a 32X
 * game is running: picodrive only ever writes the content rect it last
 * reported (see md32x_border_clear_set_content_rect), so overlay pixels left
 * outside that rect persist on whichever physical LCD buffer wasn't redrawn
 * while the overlay was up, and double buffering flickers between "clean"
 * and "overlay ghost" every other frame. See md32x_border_clear.c for the
 * exact race this fixes in the generic common_emu_state.clear_frames
 * mechanism, and tests/test_md32x_border_clear.c for the proof. */
#pragma once

#include <stdbool.h>

/* Call from emu_video_mode_change(start_line, line_count, ...): the content
 * rect can change at runtime (e.g. the VDP 30-row bit), so this must not be
 * assumed constant. */
void md32x_border_clear_set_content_rect(int top, int lines);

/* Call from the repaint callback passed to common_emu_input_loop() —
 * whenever it runs, an overlay is (or was, on its last call) on screen. */
void md32x_border_clear_notify_menu_open(void);

/* Call once per main-loop iteration, after common_emu_input_loop() returns
 * and before this frame's rendering. drawFrame must be the SAME value the
 * caller uses to decide whether to render + lcd_swap() this iteration —
 * the two clears this arms are only guaranteed to land on both physical
 * buffers if they're gated on the same drawn/skipped decision as the swap. */
void md32x_border_clear_tick(bool drawFrame);
