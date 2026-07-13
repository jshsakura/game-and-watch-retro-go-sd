/* Host unit test for Core/Src/porting/common.c: the shared per-frame pacing
 * loop every emulator core calls. The Super Metroid port never called it at
 * all (root CLAUDE.md), which is a different bug, but the pacing logic
 * itself -- the frame integrator, its debt/credit clamp, the skip_frames
 * thresholds, the overload guard, the speedup table, and
 * common_emu_sound_sync()'s DMA-half wait -- had no test before this one.
 *
 * Whole-file #include (matches tests/test_clock_alarm.c's pattern for
 * rg_clock.c): common.c's statics (frame_integrator, skip_streak) are only
 * reachable this way. Stubs below provide every symbol the TU references
 * but these tests never exercise (menu/overlay/screenshot code) -- see
 * tests/common_stubs/ for the header-only declarations that make this
 * compile. SD_CARD is left undefined (== 0 in #if), which compiles out the
 * BMP-screenshot and sdcard_hw_type branches -- neither is pacing logic. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

uint32_t g_fake_tick_ms = 0;
uint32_t dma_counter = 1;   /* start away from the "0 == uninitialized" sentinel
                             * common_emu_sound_sync()'s static last_dma_counter checks */
uint32_t audio_mute = 0;

#include "../Core/Src/porting/common.c"

/* ---- stubs: none of these are pacing logic, bodies are inert ---------- */
void SystemClock_Config(uint8_t level) { (void)level; }

static rg_app_desc_t g_app;
rg_app_desc_t *odroid_system_get_app(void) { return &g_app; }
void odroid_system_tick(uint32_t a, uint32_t b, uint32_t c) { (void)a; (void)b; (void)c; }
void odroid_system_sleep_ex(system_sleep_flags_t f, sleep_pre_wakeup_callback_t cb) { (void)f; (void)cb; }
bool odroid_system_emu_save_state(int slot) { (void)slot; return true; }
bool odroid_system_emu_load_state(int slot) { (void)slot; return true; }
char *odroid_system_get_path(emu_path_type_t t, const char *r) { (void)t; (void)r; return NULL; }

static int stub_volume = 5, stub_backlight = 5;
int  odroid_audio_volume_get(void) { return stub_volume; }
void odroid_audio_volume_set(int level) { stub_volume = level; }
int  odroid_display_get_backlight(void) { return stub_backlight; }
void odroid_display_set_backlight(int level) { stub_backlight = level; }

odroid_battery_state_t odroid_input_read_battery(void) {
    odroid_battery_state_t b = { .millivolts = 4000, .percentage = 80,
                                  .state = ODROID_BATTERY_CHARGE_STATE_DISCHARGING };
    return b;
}
void odroid_overlay_draw_battery(odroid_battery_state_t b, int x, int y) { (void)b; (void)x; (void)y; }
void odroid_overlay_sleep_pause_banner(void_callback_t r, odroid_menu_flags_t f, pause_input_callback_t cb) { (void)r; (void)f; (void)cb; }
int  odroid_overlay_game_menu(odroid_dialog_choice_t *o, void_callback_t r, odroid_menu_flags_t f) { (void)o; (void)r; (void)f; return -1; }

void audio_clear_buffers(void) {}
void audio_clear_active_buffer(void) {}
void audio_stop_playing(void) {}

bool rg_alarm_poll(void) { return false; }

static int8_t stub_turbo = 0;
bool odroid_button_turbos(void) { return false; }
int8_t odroid_settings_turbo_buttons_get(void) { return stub_turbo; }
void odroid_settings_turbo_buttons_set(int8_t t) { stub_turbo = t; }

static pixel_t fb[GW_LCD_WIDTH * GW_LCD_HEIGHT];
pixel_t *lcd_get_active_buffer(void) { return fb; }
pixel_t *lcd_get_inactive_buffer(void) { return fb; }
void lcd_sleep_while_swap_pending(void) {}
void lcd_clear_active_buffer(void) {}
void lcd_sync(void) {}
lcd_pen_t lcd_pen(uint16_t c) { (void)c; lcd_pen_t p = {0}; return p; }
void lcd_pen_set(const lcd_pen_t *p, int off) { (void)p; (void)off; }
void lcd_pen_run(const lcd_pen_t *p, int off, int count) { (void)p; (void)off; (void)count; }
void lcd_pen_darken(const lcd_pen_t *p, int off) { (void)p; (void)off; }

const unsigned char IMG_SPEAKER[1] = {0};
const unsigned char IMG_SUN[1] = {0};
const unsigned char IMG_FOLDER[1] = {0};
const unsigned char IMG_DISKETTE[1] = {0};
const unsigned char IMG_SC[1] = {0};
const unsigned char IMG_BUTTON_A[1] = {0};
const unsigned char IMG_BUTTON_A_P[1] = {0};
const unsigned char IMG_BUTTON_B[1] = {0};
const unsigned char IMG_BUTTON_B_P[1] = {0};
const unsigned char IMG_0_5X[1] = {0};
const unsigned char IMG_0_75X[1] = {0};
const unsigned char IMG_1X[1] = {0};
const unsigned char IMG_1_25X[1] = {0};
const unsigned char IMG_1_5X[1] = {0};
const unsigned char IMG_2X[1] = {0};
const unsigned char IMG_3X[1] = {0};

/* ---- test harness ------------------------------------------------------ */
static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* Full reset: struct fields (via common_emu_frame_loop_reset) + the fake
 * clock + the file-static frame_integrator/skip_streak (also zeroed by
 * common_emu_frame_loop_reset -- that's the point of using it here instead
 * of hand-zeroing common_emu_state). */
static void reset_all(uint16_t frame_time_10us, emu_speedup_t speedup) {
    common_emu_frame_loop_reset();
    common_emu_state.frame_time_10us = (int16_t)frame_time_10us;
    g_app.speedupEnabled = speedup;
    g_fake_tick_ms = 0;
    /* Burn the 3 startup_frames -- common_emu_frame_loop() skips all pacing
     * math while startup_frames < 3 (see comment in common.c), so every
     * measurement below needs to run past it first. */
    for (int i = 0; i < 3; i++) common_emu_frame_loop();
}

/* Advances the fake clock by elapsed_10us (in 10us units, matching
 * frame_time_10us's unit) and calls common_emu_frame_loop() once. */
static bool tick_frame(int32_t elapsed_10us) {
    g_fake_tick_ms += (uint32_t)(elapsed_10us / 100);
    return common_emu_frame_loop();
}

/* ---- speedup table: pin the exact scaled frame_time_10us for each enum
 * value, by bracketing the skip_frames==1 threshold (integrator > S) to a
 * single millisecond. A fresh reset means the first tick_frame's integrator
 * is just (elapsed_10us - S), so elapsed_10us == 2S is exactly ON the
 * boundary (skip_frames must stay 0) and the next whole millisecond above it
 * must cross (skip=1). NOTE: get_elapsed_time_since() is ms-resolution (it's
 * HAL_GetTick() on device) -- tick_frame() goes through the same ms-rounded
 * fake clock, so the bracket must be ms-aligned or the request gets
 * silently rounded away before common_emu_frame_loop() ever sees it. That
 * is real device behaviour, not a test artifact: sub-ms elapsed deltas
 * literally don't exist in this loop's input. */
static void test_speedup_table(void) {
    struct { emu_speedup_t su; int32_t expect_S; const char *name; } cases[] = {
        { SPEEDUP_0_5x,  2000, "0.5x" },
        { SPEEDUP_0_75x, 1250, "0.75x" },
        { SPEEDUP_1x,    1000, "1x" },
        { SPEEDUP_1_25x,  800, "1.25x" },
        { SPEEDUP_1_5x,   666, "1.5x" },
        { SPEEDUP_2x,     500, "2x" },
        { SPEEDUP_3x,     333, "3x" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int32_t S = cases[i].expect_S;
        int32_t low = (2 * S / 100) * 100;   /* <= 2S, ms-aligned: integrator <= S -> skip_frames=0 */
        int32_t high = low + 100;            /* one ms above: integrator > S -> skip_frames=1 */

        reset_all(1000, cases[i].su);
        tick_frame(low);
        CHECK(common_emu_state.skip_frames == 0,
              "speedup %s: elapsed=%d (<=2*S=%d) should NOT skip yet (skip_frames=%d)",
              cases[i].name, low, S, common_emu_state.skip_frames);

        reset_all(1000, cases[i].su);
        tick_frame(high);
        CHECK(common_emu_state.skip_frames == 1,
              "speedup %s: elapsed=%d should cross into skip_frames=1 (got %d) -- pins scaled frame_time=%d",
              cases[i].name, high, common_emu_state.skip_frames, S);
    }
    printf("  speedup table: 7 enum values x 2 boundary checks\n");
}

/* ---- skip_frames 0/1/2 thresholds and the pause_frames branch --------- */
static void test_thresholds(void) {
    /* integrator in (S, 2S] -> skip_frames = 1 */
    reset_all(1000, SPEEDUP_1x);
    tick_frame(1000 + 1500); /* integrator = 1500 */
    CHECK(common_emu_state.skip_frames == 1, "integrator=1500,S=1000 should be skip_frames=1 (got %d)", common_emu_state.skip_frames);
    CHECK(common_emu_state.pause_frames == 0, "skip_frames=1 case must not also set pause_frames");

    /* integrator > 2S (but under the 2.5S debt cap) -> skip_frames = 2 */
    reset_all(1000, SPEEDUP_1x);
    tick_frame(1000 + 2200); /* integrator = 2200, cap is 2500 so this isn't clamped */
    CHECK(common_emu_state.skip_frames == 2, "integrator=2200,S=1000 should be skip_frames=2 (got %d)", common_emu_state.skip_frames);

    /* integrator < -S (credit) -> pause_frames = 1. Reached over two ticks
     * with elapsed=0 (no time passed that frame) rather than a "negative
     * elapsed" trick -- get_elapsed_time_since() is unsigned, negative
     * elapsed isn't a real input this loop ever sees on device. */
    reset_all(1000, SPEEDUP_1x);
    tick_frame(0); /* integrator = -1000 (== -S, not yet < -S) */
    CHECK(common_emu_state.pause_frames == 0, "integrator==-S must not yet pause (got pause_frames=%d)", common_emu_state.pause_frames);
    tick_frame(0); /* integrator = -2000, clamped by credit_cap to -1500, still < -S */
    CHECK(common_emu_state.pause_frames == 1, "integrator<-S should set pause_frames=1 (got %d)", common_emu_state.pause_frames);
    CHECK(common_emu_state.skip_frames == 0, "pause case must not also set skip_frames");

    printf("  thresholds: skip_frames 0/1/2 + pause_frames branch\n");
}

/* ---- the debt clamp: without it, a single long stall banks a debt that
 * takes hundreds of frames-at-pace to work off (the Dynastic Hero 30s
 * freeze in common.c's own comment). With the 2.5x cap, 1-2 frames that run
 * faster than budget fully recover it. This is the regression test named in
 * the task -- see the RED/GREEN transcript in the final report, produced by
 * temporarily deleting the clamp block and rerunning this exact test. ---- */
static void test_debt_clamp_recovery(void) {
    reset_all(1000, SPEEDUP_1x);
    tick_frame(3000 * 100); /* a 3-second stall: elapsed_10us = 300000 */
    CHECK(common_emu_state.skip_frames == 2, "post-stall frame must be skip_frames=2 (got %d)", common_emu_state.skip_frames);

    /* Two frames that come in with zero elapsed time (running flat-out,
     * catching up) must be enough to fully recover if the debt was capped
     * at 2.5x frame_time instead of banking the whole 300000-unit stall. */
    tick_frame(0); /* integrator: 2500 (clamped) - 1000 = 1500 -> skip_frames=1 */
    CHECK(common_emu_state.skip_frames == 1, "1 fast frame after the clamp should drop to skip_frames=1 (got %d) -- clamp not capping debt?", common_emu_state.skip_frames);
    tick_frame(0); /* integrator: 1500 - 1000 = 500 -> skip_frames=0, recovered */
    CHECK(common_emu_state.skip_frames == 0, "2 fast frames after the clamp should fully recover (got skip_frames=%d) -- screen would still read frozen", common_emu_state.skip_frames);

    printf("  debt clamp: 3s stall recovers in 2 frames (clamp caps banked debt at 2.5x)\n");
}

/* ---- overload guard: once skip_frames is pinned at 2, force one drawn
 * frame every 4 so the worst case is ~15fps visible, never a flat 0. ------ */
static void test_overload_guard(void) {
    reset_all(1000, SPEEDUP_1x);
    /* Pin skip_frames=2 by feeding elapsed=3500 every frame: delta=+2500
     * saturates the debt cap (2500) on the very first call and keeps
     * re-saturating it every call after. */
    tick_frame(3500); /* establishes skip_frames=2 for the next call to read */
    CHECK(common_emu_state.skip_frames == 2, "setup: expected skip_frames pinned at 2");

    bool draws[5];
    for (int i = 0; i < 5; i++) draws[i] = tick_frame(3500);

    /* draw_frame reflects the PREVIOUS call's skip_frames, so with
     * skip_frames pinned at 2 throughout, only the forced 1-in-4 frame
     * should come back true. */
    bool expect[5] = { false, false, false, true, false };
    for (int i = 0; i < 5; i++) {
        CHECK(draws[i] == expect[i], "overload guard call %d: expected draw=%d got %d", i, expect[i], draws[i]);
    }
    printf("  overload guard: F,F,F,T,F pattern (1 drawn frame per 4 skipped)\n");
}

/* ---- common_emu_sound_sync(): must not wait when skip_frames is set (a
 * slow core catching up would otherwise stall its own catch-up mechanism),
 * and must wait exactly pause_frames+1 DMA halves otherwise. __NOP()/__WFI()
 * (tests/common_stubs/main.h) advance dma_counter once per call, so the
 * delta after common_emu_sound_sync() returns is exactly the number of
 * DMA-half waits it performed. --------------------------------------------- */
static void test_sound_sync(void) {
    common_emu_state.skip_frames = 1;
    common_emu_state.pause_frames = 0;
    uint32_t before = dma_counter;
    common_emu_sound_sync(true);
    CHECK(dma_counter == before, "skip_frames set: sound_sync must not wait (dma_counter moved by %u)", dma_counter - before);

    common_emu_state.skip_frames = 2;
    before = dma_counter;
    common_emu_sound_sync(true);
    CHECK(dma_counter == before, "skip_frames=2: sound_sync must not wait (dma_counter moved by %u)", dma_counter - before);

    common_emu_state.skip_frames = 0;
    common_emu_state.pause_frames = 0;
    before = dma_counter;
    common_emu_sound_sync(true);
    CHECK(dma_counter - before == 1, "pause_frames=0: expected 1 DMA-half wait, got %u", dma_counter - before);

    common_emu_state.skip_frames = 0;
    common_emu_state.pause_frames = 1;
    before = dma_counter;
    common_emu_sound_sync(true);
    CHECK(dma_counter - before == 2, "pause_frames=1: expected 2 DMA-half waits, got %u", dma_counter - before);

    /* use_nops=false takes the cpumon_sleep()/__WFI() path instead -- same
     * contract, different macro. */
    common_emu_state.skip_frames = 0;
    common_emu_state.pause_frames = 0;
    before = dma_counter;
    common_emu_sound_sync(false);
    CHECK(dma_counter - before == 1, "use_nops=false, pause_frames=0: expected 1 DMA-half wait, got %u", dma_counter - before);

    printf("  sound_sync: skip_frames skips the wait; waits pause_frames+1 DMA halves otherwise\n");
}

int main(void) {
    printf("=== common.c: frame loop + speedup table + sound_sync ===\n");
    test_speedup_table();
    test_thresholds();
    test_debt_clamp_recovery();
    test_overload_guard();
    test_sound_sync();

    if (failures) {
        printf("%d assertion(s) FAILED\n", failures);
        return 1;
    }
    printf("all common.c assertions passed\n");
    return 0;
}
