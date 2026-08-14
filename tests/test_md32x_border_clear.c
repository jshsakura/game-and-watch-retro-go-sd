/* Host unit test for Core/Src/porting/md32x/md32x_border_clear.c: proves the
 * exact race md32x_border_clear.h documents. The generic
 * common_emu_state.clear_frames mechanism (common.c) decrements once per
 * LOOP ITERATION regardless of whether that iteration actually draws +
 * lcd_swap()s -- under 32X's frame-skip pacing, two skipped iterations right
 * after menu close can both land on the SAME still-active physical buffer,
 * leaving the other one's border rows stuck with stale overlay pixels
 * forever (the visible symptom: the top/bottom bands flicker between
 * "clean" and "overlay ghost" every other frame, since lcd_swap() keeps
 * alternating which of the two buffers is shown).
 *
 * Whole-file #include (matches tests/test_common.c's pattern) so the file's
 * static content-rect/counter state is exercised through its real
 * transitions, not reimplemented here. Stubs below model a minimal 2-buffer
 * LCD: lcd_get_active_buffer()/lcd_sleep_while_swap_pending() are the only
 * two functions md32x_border_clear.c calls, from tests/common_stubs/gw_lcd.h
 * (reused, not duplicated). */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#define MD32X_FB_WIDTH  320
#define MD32X_FB_HEIGHT 240

static uint16_t fb_a[MD32X_FB_WIDTH * MD32X_FB_HEIGHT];
static uint16_t fb_b[MD32X_FB_WIDTH * MD32X_FB_HEIGHT];
static int active_is_a = 1;   /* which physical buffer is "active" right now */

uint16_t *lcd_get_active_buffer(void) { return active_is_a ? fb_a : fb_b; }
uint16_t *lcd_get_inactive_buffer(void) { return active_is_a ? fb_b : fb_a; }
void lcd_sleep_while_swap_pending(void) { /* no pending-swap model needed: the test never overlaps a "swap in flight" with a clear */ }

/* NOT called by md32x_border_clear.c -- only present so gw_lcd.h's other
 * declarations still link if something pulls them in transitively. */
void lcd_clear_active_buffer(void) { memset(lcd_get_active_buffer(), 0, sizeof(fb_a)); }
void lcd_sync(void) {}

#include "../Core/Src/porting/md32x/md32x_border_clear.c"

/* Mirrors the main loop's own contract (main_md32x.c): lcd_swap() happens
 * if and only if drawFrame was true, exactly once, AFTER
 * md32x_border_clear_tick() ran for that iteration -- same order the real
 * loop uses. */
static void sim_iteration(bool drawFrame) {
    md32x_border_clear_tick(drawFrame);
    if (drawFrame)
        active_is_a = !active_is_a;
}

/* content marker: DIRTY in the border, GAME in the content rect. A pass
 * that only clears the border must turn DIRTY -> 0 and leave GAME alone. */
#define DIRTY 0xBEEF
#define GAME  0x1234

static void paint_dirty_borders(uint16_t *fb, int content_top, int content_lines) {
    for (int y = 0; y < MD32X_FB_HEIGHT; y++) {
        bool in_content = y >= content_top && y < content_top + content_lines;
        uint16_t v = in_content ? GAME : DIRTY;
        for (int x = 0; x < MD32X_FB_WIDTH; x++)
            fb[y * MD32X_FB_WIDTH + x] = v;
    }
}

static bool borders_clean(const uint16_t *fb, int content_top, int content_lines) {
    for (int y = 0; y < MD32X_FB_HEIGHT; y++) {
        bool in_content = y >= content_top && y < content_top + content_lines;
        uint16_t want = in_content ? GAME : 0;
        for (int x = 0; x < MD32X_FB_WIDTH; x++)
            if (fb[y * MD32X_FB_WIDTH + x] != want)
                return false;
    }
    return true;
}

static void reset_state(void) {
    md32x_content_top = 8;
    md32x_content_lines = 224;
    md32x_menu_was_open = false;
    md32x_border_clear_frames = 0;
    active_is_a = 1;
}

int main(void) {
    int rc = 0;

    /* --- 1: steady state (no menu ever opened) touches nothing. This is
     * the "normal-state cost is zero" property -- not just cheap, but a
     * hard zero: no memset call happens at all when clear_frames is 0. */
    reset_state();
    paint_dirty_borders(fb_a, 8, 224);
    paint_dirty_borders(fb_b, 8, 224);
    for (int i = 0; i < 10; i++) sim_iteration(true);
    if (fb_a[0] != DIRTY || fb_b[0] != DIRTY) {
        printf("FAIL steady-state: border touched without a menu ever opening\n");
        rc = 1;
    } else {
        printf("OK steady-state: no menu open -> zero clears\n");
    }

    /* --- 2: the adversarial race this module exists to fix. Menu closes,
     * then TWO CONSECUTIVE SKIPPED frames (drawFrame=false) -- the exact
     * pattern that breaks the generic per-loop-iteration clear_frames
     * counter, because no lcd_swap() happens between them. Both physical
     * buffers start with dirty borders (worst case: neither was redrawn
     * while the menu was up). */
    reset_state();
    paint_dirty_borders(fb_a, 8, 224);
    paint_dirty_borders(fb_b, 8, 224);
    md32x_border_clear_notify_menu_open();     /* md32x_repaint() would call this */
    bool seq[] = { false, false, true, true, true };
    for (size_t i = 0; i < sizeof(seq)/sizeof(seq[0]); i++)
        sim_iteration(seq[i]);
    bool a_clean = borders_clean(fb_a, 8, 224);
    bool b_clean = borders_clean(fb_b, 8, 224);
    if (!a_clean || !b_clean) {
        printf("FAIL race scenario: buf_a_clean=%d buf_b_clean=%d (expected both 1)\n",
               a_clean, b_clean);
        rc = 1;
    } else {
        printf("OK race scenario: both physical buffers' borders clean, content untouched, "
               "despite two skipped frames right after menu close\n");
    }

    /* --- 3: menu closes with NO skips at all (the easy case) -- must also
     * clean both buffers, and must NOT touch the content rows either. */
    reset_state();
    paint_dirty_borders(fb_a, 8, 224);
    paint_dirty_borders(fb_b, 8, 224);
    md32x_border_clear_notify_menu_open();
    sim_iteration(true);
    sim_iteration(true);
    if (!borders_clean(fb_a, 8, 224) || !borders_clean(fb_b, 8, 224)) {
        printf("FAIL no-skip scenario: borders not clean on both buffers\n");
        rc = 1;
    } else {
        printf("OK no-skip scenario: both buffers clean, content preserved\n");
    }

    /* --- 4: dynamic content rect (VDP 30-row mode, PicoFrameStart's
     * loffs=0/lines=240) -- full-height content, so there is NO border to
     * clear. Must be a no-op even right after a menu close, or it would
     * blank live game content instead of an unused margin. */
    reset_state();
    md32x_border_clear_set_content_rect(0, 240);
    for (int i = 0; i < MD32X_FB_WIDTH * MD32X_FB_HEIGHT; i++)
        fb_a[i] = fb_b[i] = GAME;   /* whole screen is "content" here -- no border exists */
    md32x_border_clear_notify_menu_open();
    sim_iteration(true);
    sim_iteration(true);
    if (fb_a[0] != GAME || fb_b[MD32X_FB_WIDTH*(MD32X_FB_HEIGHT-1)] != GAME) {
        printf("FAIL 30-row mode: full-height content was clobbered (no border exists to clear)\n");
        rc = 1;
    } else {
        printf("OK 30-row mode (content_top=0, content_lines=240): no border, no-op\n");
    }

    return rc;
}
