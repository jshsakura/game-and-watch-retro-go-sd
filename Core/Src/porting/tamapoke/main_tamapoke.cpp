/* TamaPoke app entry and frame loop.
 *
 * Upstream is an Arduino sketch: setup() then a free-running loop() that
 * blocks on I2S and polls a touch panel. Here the loop is paced by the shared
 * frame loop, input comes from the focus layer, and audio is pulled once a
 * frame instead of pushed by a task.
 */
#include <stdint.h>

#include <Preferences.h>

#include "dex.h"
#include "species.h"
#include "tamapoke_assets.h"
#include "tamapoke_audio.h"
#include "tamapoke_input.h"
#include "tamapoke_shim.h"
#include "tamapoke_sprites.h"
#include "tamapoke_ui.h"

extern "C" {
#include "appid.h"
#include "cpp_init_array.h"
#include "gw_linker.h"
#include "common.h"
#include "gw_audio.h"
#include "gw_lcd.h"
#include "main.h"
#include "odroid_system.h"
}

/* Upstream dims the panel and then blanks it on its own timer. The launcher
 * already owns that policy, and a screen that ignores it sits lit for ever at
 * any setting -- which is exactly how the clock app shipped. So the port asks
 * the one rule instead of re-deriving it. */
#define IDLE_POLL_SECONDS 1

/* Sized to the system buffer, not to our nominal frame's worth: the shared
 * audio path decides how many samples it wants each frame, and it is the one
 * that has to be filled exactly. */
static int16_t audio_buffer[AUDIO_BUFFER_LENGTH];

/* The pet writes its state constantly; the card must not be touched mid-play.
 * Persistence happens here, at the points where a stall is harmless and the
 * user is not mid-interaction. */
static void commit_save(void) { tamapoke_prefs_commit(); }

extern "C" void app_main_tamapoke(uint8_t load_state, uint8_t start_paused, int8_t save_slot) {
  (void)load_state;
  (void)save_slot;

  /* Run this overlay's C++ constructors, now that the overlay is actually in
   * RAM. __libc_init_array() cannot do it: it runs at boot, from resident code,
   * when RAM_EMU holds nothing -- so the linker script keeps our .init_array
   * inside the overlay and we call it here, the way every other C++ core does
   * (lynx, a2600, c64, tgbdual). Skipping this step is not a missing feature,
   * it is a black screen on boot: see the comment in STM32H7B0VBTx_SDCARD.ld. */
  cpp_init_array(__init_array_tamapoke_start__, __init_array_tamapoke_end__);

  common_emu_state.frame_time_10us = (uint16_t)(100000 / TAMAPOKE_FPS);
  common_emu_state.pause_after_frames = start_paused ? 2 : 0;

  odroid_system_init(APPID_HOMEBREW, TAMAPOKE_SAMPLE_RATE);

  tamapoke_shim_init();
  audioBegin();
  sdReady = tamapoke_assets_open();
  /* Species names live on the card, not in the binary -- see dex.h. Missing
   * names are not fatal: everything reads as "#NNN" and the game still plays. */
  tamapoke_dex_load_names();
  tamapoke_load_fallback_sprites();
  thumbs.load();

  lcd_clear_buffers();
  tamapoke_ui_init();

  audio_start_playing(TAMAPOKE_AUDIO_BUFFER_LENGTH);

  uint32_t idle_start = uptime_get();
  uint32_t last_activity_focus = 0;

  while (true) {
    wdog_refresh();

    bool draw_frame = common_emu_frame_loop();
    uint32_t now = tamapoke_millis();

    tamapoke_input_poll(now);
    tamapoke_ui_tick(now);

    if (draw_frame) {
      tamapoke_ui_render();
      /* Present, then bring the presented frame back into the new active
       * buffer. The UI redraws incrementally and expects the canvas to keep
       * what it drew last frame; without the clone each swap would expose the
       * frame before last. */
      lcd_swap();
      lcd_clone();
    }

    if (!common_emu_sound_loop_is_muted()) {
      uint16_t n = audio_get_buffer_length();
      if (n > AUDIO_BUFFER_LENGTH) n = AUDIO_BUFFER_LENGTH;
      tamapoke_audio_fill(audio_buffer, n);

      int32_t factor = common_emu_sound_get_volume();
      int16_t *out = audio_get_active_buffer();
      for (uint16_t i = 0; i < n; i++) out[i] = (int16_t)((audio_buffer[i] * factor) >> 8);
    }

    /* Any focus movement or action counts as activity, which is cheaper and
     * more honest than re-reading the gamepad here. */
    uint8_t focus = tamapoke_input_focus();
    if (focus != last_activity_focus || tamapoke_ui_had_activity()) {
      last_activity_focus = focus;
      idle_start = uptime_get();
    }

    if (odroid_idle_timeout_expired(uptime_get() - idle_start)) {
      commit_save();
      odroid_system_sleep();
      idle_start = uptime_get();
    }

    common_emu_sound_sync(false);
  }
}
