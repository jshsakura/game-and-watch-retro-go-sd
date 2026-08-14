#pragma once

#include <odroid_system.h>

#include "main.h"
#include "gw_lcd.h"
#include "gw_audio.h"

#define WIDTH  320
#define HEIGHT 240
#define BPP      4

/* odroid_dialog_choice_t.enabled convention extension: the submodule
 * (retro-go-stm32/components/odroid/odroid_overlay.h, upstream-pinned — not
 * ours to edit) only documents >=1 selectable / -1 disabled-but-drawn-greyed.
 * This project's odroid_overlay_dialog (Core/Src/porting/odroid_overlay.c)
 * additionally treats this exact sentinel as "fully omitted": zero layout
 * height, never drawn, never a navigation stop (odroid_overlay_dialog_find_
 * next_item already skips any enabled < 1, so hidden rows are skipped there
 * for free — only the layout/draw loops needed the extra check). Distinct
 * from plain -1. Toggle it live from a sibling row's update_cb exactly like
 * -1 already is (see rg_clock.c cb_anim). */
#define ODROID_DIALOG_HIDDEN (-2)

extern const uint8_t volume_tbl[ODROID_AUDIO_VOLUME_MAX + 1];

void common_emu_frame_loop_reset(void);
bool common_emu_frame_loop(void);
/* Frames emulated, and frames that actually reached the panel, for every core.
 * Read over SWD by tools/gnw_probe/drawn_ab.sh; see common.c for why emulated
 * fps alone is the wrong instrument whenever the overload guard is engaged.
 *
 * They are counted in two different places on purpose: _emu_ in
 * common_emu_frame_loop(), _drawn_ in lcd_swap(). Counting _drawn_ from the
 * guard's `draw_frame` recorded a DECISION several cores discard (main_vb.c
 * always presents, main_wswan.c forces every sixth skip, main_videopac.c never
 * reads it) and produced a VB reading of 9 fps for a core presenting 36. The
 * three cores that flip WITHOUT new content call lcd_swap_stale() instead --
 * see the two comments in gw_lcd.c. Because lcd_swap() is also the launcher's
 * and the clock's, _drawn_ is only meaningful as a delta with a game running. */
extern uint32_t g_common_drawn_frames;
extern uint32_t g_common_emu_frames;
void common_emu_input_loop(odroid_gamepad_state_t *joystick, odroid_dialog_choice_t *game_options, void_callback_t repaint);
void common_emu_input_loop_handle_turbo(odroid_gamepad_state_t *joystick);
void common_emu_sound_sync(bool use_nops);
void common_emu_sound_sync_reset(void);
bool common_emu_sound_loop_is_muted();
uint8_t common_emu_sound_get_volume();

/* DMA half-buffer pacing marker shared by common_emu_sound_sync and PCE. */
extern uint32_t common_emu_sound_dma_marker;

typedef struct {
    uint last_busy;
    uint busy_ms;
    uint sleep_ms;
} cpumon_stats_t;
extern cpumon_stats_t cpumon_stats;

/**
 * Just calls `__WFI()` and measures time spent sleeping.
 */
void cpumon_sleep(void);
void cpumon_busy(void);
void cpumon_reset(void);


enum {
    INGAME_OVERLAY_NONE,
    INGAME_OVERLAY_VOLUME,
    INGAME_OVERLAY_BRIGHTNESS,
    INGAME_OVERLAY_SAVE,
    INGAME_OVERLAY_LOAD,
    INGAME_OVERLAY_SPEEDUP,
    INGAME_OVERLAY_SC,
    INGAME_OVERLAY_BUTTON_A,
    INGAME_OVERLAY_BUTTON_B,
};
typedef uint8_t ingame_overlay_t;

/**
 * Holds common higher-level emu options that need to be used at not-neat
 * locations in each emulator.
 *
 * There should only be one of these objects instantiated.
 */
typedef struct {
    uint32_t last_sync_time;
    uint32_t last_overlay_time;
    uint16_t skipped_frames;
    int16_t frame_time_10us;
    uint8_t skip_frames:2;
    uint8_t pause_frames:1;
    uint8_t pause_after_frames:3;
    uint8_t startup_frames:2;
    uint8_t overlay:4;
    uint8_t clear_frames:2;
} common_emu_state_t;

extern common_emu_state_t common_emu_state;

// DWT start
void common_emu_enable_dwt_cycles(void);
unsigned int common_emu_get_dwt_cycles(void);
void common_emu_clear_dwt_cycles(void);
// DWT end

/**
 * Drawable stuff over current emulation.
 */
void common_ingame_overlay(void);
void common_emu_auto_oc(uint8_t level);   /* per-system CPU boost, see common.c */

void draw_darken_rounded_rectangle(pixel_t *fb, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);

/**
 * Draw border screen for Zelda 3 when not full screen.
 */
void draw_border_zelda3(pixel_t * fb);

/**
 * Draw border screen for Super Mario World.
 */
void draw_border_smw(pixel_t * fb);