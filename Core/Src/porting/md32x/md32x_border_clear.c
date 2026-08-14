/* See md32x_border_clear.h for the bug this fixes and the call contract.
 *
 * Root cause: the generic common_emu_state.clear_frames mechanism
 * (Core/Src/porting/common.c) already exists to clear stale overlay pixels
 * after menu close, but it decrements once per LOOP ITERATION regardless of
 * whether that iteration actually draws + lcd_swap()s. Under 32X's
 * frame-skip pacing (common_emu_frame_loop's overload guard allows runs of
 * skipped frames), two skipped iterations in a row can both decrement while
 * the active buffer is the SAME physical one (no swap happened between
 * them) — leaving the OTHER buffer's border rows never cleared. This module
 * fixes that by gating its own counter on the caller's drawFrame value: a
 * clear only counts down on an iteration that WILL swap, so two counted
 * clears are guaranteed to land on the two distinct physical buffers.
 *
 * Deliberately its own small dependency-free TU (only gw_lcd.h) rather than
 * living inline in main_md32x.c, so it can be unit-tested by compiling this
 * exact file (tests/test_md32x_border_clear.c) instead of the whole
 * picodrive-core-linked main_md32x.c. */
#include "md32x_border_clear.h"

#include <stdint.h>
#include <string.h>

#include "gw_lcd.h"

#define MD32X_FB_WIDTH  320
#define MD32X_FB_HEIGHT 240

/* picodrive's own PicoFrameStart() defaults (loffs=8, lines=224 — V28/28-row
 * mode) until the first real report arrives. NOT assumed constant elsewhere:
 * PicoFrameStart flips to loffs=0, lines=240 (no border at all) when a game
 * sets the VDP 30-row bit (Pico.video.reg[1]&8). 32X's own column geometry
 * is always the full 320 (PAHW_32X skips PicoFrameStart's H32-narrowing
 * branch), so only the top/bottom rows can ever be border. */
static int md32x_content_top = 8, md32x_content_lines = 224;

static bool md32x_menu_was_open = false;
static int md32x_border_clear_frames = 0;

void md32x_border_clear_set_content_rect(int top, int lines) {
  md32x_content_top = top;
  md32x_content_lines = lines;
}

void md32x_border_clear_notify_menu_open(void) {
  md32x_menu_was_open = true;
}

static void md32x_clear_border_rows(void) {
  /* Same race lcd_clear_active_buffer() guards against (the lcd_clear_buffers()
   * ban, main_md32x.c's emu_video_mode_change): a pending swap from the
   * previous drawn frame may not have landed at the next vblank yet, so this
   * buffer could still be mid-scanout. Wait first — returns immediately in
   * the normal cadence. */
  lcd_sleep_while_swap_pending();
  uint16_t *fb = lcd_get_active_buffer();
  if (md32x_content_top > 0)
    memset(fb, 0, (size_t)md32x_content_top * MD32X_FB_WIDTH * sizeof(uint16_t));
  int bottom_start = md32x_content_top + md32x_content_lines;
  if (bottom_start < MD32X_FB_HEIGHT)
    memset(fb + (size_t)bottom_start * MD32X_FB_WIDTH, 0,
           (size_t)(MD32X_FB_HEIGHT - bottom_start) * MD32X_FB_WIDTH * sizeof(uint16_t));
}

void md32x_border_clear_tick(bool drawFrame) {
  if (md32x_menu_was_open) {
    md32x_menu_was_open = false;
    md32x_border_clear_frames = 2;
  }
  if (drawFrame && md32x_border_clear_frames > 0) {
    md32x_border_clear_frames--;
    md32x_clear_border_rows();
  }
}
