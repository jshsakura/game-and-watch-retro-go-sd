/* Host-test stub, mirrors retro-go-stm32/components/odroid/odroid_system.h
 * (upstream-pinned submodule) for the pieces common.c actually touches.
 *
 * get_elapsed_time()/get_elapsed_time_since() are the fake clock the task
 * asked for: the real header just wraps HAL_GetTick(), so here they read a
 * test-controlled tick directly -- same shape, no HAL needed. */
#ifndef STUB_ODROID_SYSTEM_H
#define STUB_ODROID_SYSTEM_H
#include <stdint.h>
#include <stdbool.h>
#include "odroid_input.h"
#include "odroid_overlay.h"

#define ODROID_AUDIO_VOLUME_MIN 0
#define ODROID_AUDIO_VOLUME_MAX 9
#define ODROID_BACKLIGHT_LEVEL_COUNT 10

enum {
    SPEEDUP_MIN = -3,
    SPEEDUP_0_5x = -2,
    SPEEDUP_0_75x = -1,
    SPEEDUP_1x = 0,
    SPEEDUP_1_25x,
    SPEEDUP_1_5x,
    SPEEDUP_2x,
    SPEEDUP_3x,
    SPEEDUP_MAX,
};
typedef int32_t emu_speedup_t;

typedef struct {
    const char *romPath;
    emu_speedup_t speedupEnabled;
} rg_app_desc_t;

typedef enum {
    ODROID_PATH_USER_SCREENSHOT,
} emu_path_type_t;

typedef enum {
    SLEEP_SHOW_ANIMATION = 1 << 0,
    SLEEP_ENTER_SLEEP = 1 << 2,
} system_sleep_flags_t;
typedef void (*sleep_pre_wakeup_callback_t)(void);

rg_app_desc_t *odroid_system_get_app(void);
void odroid_system_tick(uint32_t skippedFrame, uint32_t fullFrame, uint32_t busyTime);
void odroid_system_sleep_ex(system_sleep_flags_t flags, sleep_pre_wakeup_callback_t cb);
bool odroid_system_emu_save_state(int slot);
bool odroid_system_emu_load_state(int slot);
char *odroid_system_get_path(emu_path_type_t type, const char *romPath);

int  odroid_audio_volume_get(void);
void odroid_audio_volume_set(int level);
int  odroid_display_get_backlight(void);
void odroid_display_set_backlight(int level);

/* fake clock -- test sets g_fake_tick_ms directly (HAL_GetTick() units: ms) */
extern uint32_t g_fake_tick_ms;
static inline uint32_t get_elapsed_time(void) { return g_fake_tick_ms; }
static inline uint32_t get_elapsed_time_since(uint32_t start) { return get_elapsed_time() - start; }

#endif
