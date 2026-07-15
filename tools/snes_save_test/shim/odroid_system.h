/* Host shim for the SNES savestate cold-resume proof.
 *
 * One header satisfies every firmware include main_snes.c pulls in (each
 * firmware header name in this dir just includes this one). The point of the
 * exercise: compile Core/Src/porting/snes/main_snes.c ITSELF — its serializer,
 * its stamp, its event loop — so the test cannot drift from the device code
 * (the hw_jpeg lesson: a test must compile the thing it tests).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- input ---- */
enum {
  ODROID_INPUT_UP, ODROID_INPUT_RIGHT, ODROID_INPUT_DOWN, ODROID_INPUT_LEFT,
  ODROID_INPUT_SELECT, ODROID_INPUT_START, ODROID_INPUT_A, ODROID_INPUT_B,
  ODROID_INPUT_X, ODROID_INPUT_Y, ODROID_INPUT_POWER, ODROID_INPUT_VOLUME,
  ODROID_INPUT_MAX
};
typedef struct { uint8_t values[ODROID_INPUT_MAX]; } odroid_gamepad_state_t;
void odroid_input_read_gamepad(odroid_gamepad_state_t *out);

/* ---- dialog / overlay ---- */
typedef struct {
  int id; const char *label; char value[32]; int enabled; void *update_cb;
} odroid_dialog_choice_t;
#define ODROID_DIALOG_CHOICE_LAST {0x0F0F0F0F, "LAST", "LAST", 0xFFFF, NULL}
void odroid_overlay_alert(const char *msg);
const uint8_t *odroid_overlay_cache_file_in_flash(const char *path, uint32_t *size, bool exec);

/* ---- system ---- */
typedef bool (*state_fn_t)(const char *pathName);
typedef void *(*screenshot_fn_t)(void);
void odroid_system_init(int appId, int sampleRate);
void odroid_system_emu_init(state_fn_t load, state_fn_t save, screenshot_fn_t ss,
                            void *a, void *b, void *c);
void odroid_system_emu_load_state(int slot);
void odroid_system_switch_app(int app);
void odroid_audio_mute(bool mute);

/* ---- common emu loop ---- */
typedef struct {
  uint16_t pause_after_frames;
  uint16_t frame_time_10us;
} common_emu_state_t;
extern common_emu_state_t common_emu_state;
bool common_emu_frame_loop(void);
void common_emu_input_loop(odroid_gamepad_state_t *joy, odroid_dialog_choice_t *opt,
                           void (*repaint)(void));
void common_emu_input_loop_handle_turbo(odroid_gamepad_state_t *joy);
void common_emu_sound_sync(bool late);
bool common_emu_sound_loop_is_muted(void);
int32_t common_emu_sound_get_volume(void);
void common_emu_auto_oc(int level);
void common_ingame_overlay(void);

/* ---- lcd ---- */
uint16_t *lcd_get_active_buffer(void);
void lcd_swap(void);
void lcd_clear_buffers(void);
void lcd_clear_active_buffer(void);
void lcd_wait_for_vblank(void);
void lcd_set_refresh_rate(int hz);

/* ---- audio ---- */
void audio_start_playing(int samples_per_frame);
int16_t *audio_get_active_buffer(void);
uint16_t audio_get_buffer_length(void);

/* ---- misc firmware ---- */
void wdog_refresh(void);
void *itc_malloc(size_t s);
void *itc_calloc(size_t n, size_t s);
void *ahb_malloc(size_t s);
void *ram_malloc(size_t s);
void *ram_calloc(size_t n, size_t s);

/* ---- rom manager ---- */
typedef struct { const char *path; } retro_emulator_file_t;
extern retro_emulator_file_t *ACTIVE_FILE;

#define APPID_SNES 0x5E5  /* value irrelevant on host */
