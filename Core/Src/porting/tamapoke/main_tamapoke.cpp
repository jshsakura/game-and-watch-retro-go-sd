/* TamaPoke app entry and frame loop.
 *
 * Upstream is an Arduino sketch: setup() then a free-running loop() that
 * blocks on I2S and polls a touch panel. Here the loop is paced by the shared
 * frame loop, input comes from the focus layer, and audio is pulled once a
 * frame instead of pushed by a task.
 */
#include <stdint.h>
#include <string.h>

#include <Preferences.h>

#include "dex.h"
#include "i18n.h"
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
/* The launcher's own i18n, for the pause-menu row label (curr_lang->s_LangUI).
 * Distinct from this port's i18n.h above: that one holds TamaPoke's strings, this
 * one holds the launcher's. */
#include "rg_i18n.h"
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

/* The pause menu repaints the screen behind itself through this. The UI draws
 * incrementally, so a full re-render is the only correct answer -- anything less
 * leaves the menu's own pixels behind when it closes. */
static void repaint_ui(void) {
  /* The menu painted over the canvas, so an incremental redraw would leave its
   * pixels wherever this frame does not draw. Repaint in full. */
  tamapoke_ui_force_full_repaint();
  tamapoke_ui_render();
}

/* Language, chosen from the launcher's pause menu the way Zelda 3 offers its own:
 * LEFT/RIGHT walk the list and the change lands immediately -- setLang() applies
 * it, persists it, and repoints the species-name table at the new language's
 * names on the card, so the screen behind the menu is already in the new language
 * when it closes.
 *
 * This is where a player looks for it. TamaPoke's own settings screen has
 * upstream's language pill and that still works, but it is two swipes away and
 * nobody found it. No new screen was needed -- just a row in the menu that is
 * already there. */
static char lang_value[8];

static bool lang_update_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event,
                           uint32_t repeat) {
  (void)repeat;
  int last = LANG_COUNT - 1;
  int idx = (int)gLang;

  if (event == ODROID_DIALOG_PREV) idx = (idx > 0) ? idx - 1 : last;
  if (event == ODROID_DIALOG_NEXT) idx = (idx < last) ? idx + 1 : 0;

  if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT)
    setLang((Lang)idx);

  strncpy(option->value, langName(gLang), sizeof(lang_value) - 1);
  option->value[sizeof(lang_value) - 1] = '\0';
  return event == ODROID_DIALOG_ENTER;
}

/* The pause menu's Save rows go through these. A virtual pet has no snapshot to
 * take: its whole state is the preferences blob it already writes at safe points,
 * and the card is the save. So "save" means commit that now, and "load" is a
 * no-op that must still report success -- returning false would make the menu
 * announce a failure for something that is not broken.
 *
 * They are wired rather than left out because the menu offers the rows either
 * way: Super Metroid shipped this exact gap, with Save and Load doing nothing at
 * all because odroid_system_emu_init() was never called. A row that lies is
 * worse than a row that is absent. */
static bool tamapoke_save_state(const char *path) {
  (void)path;
  tamapoke_prefs_commit();
  return true;
}

static bool tamapoke_load_state(const char *path) {
  (void)path;
  return true;  /* prefs are loaded at startup; there is nothing else to restore */
}

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
  /* Register the state hooks before the menu can be opened, or its Save rows
   * are decoration. */
  odroid_system_emu_init(&tamapoke_load_state, &tamapoke_save_state, NULL, NULL, NULL, NULL, NULL);

  tamapoke_shim_init();
  audioBegin();
  sdReady = tamapoke_assets_open();
  /* Restore the saved UI language BEFORE the names load, since which name table
   * is read depends on it. Nothing called this, so every launch came up in the
   * default (English) however the pill had been left. */
  loadLang();
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

  strncpy(lang_value, langName(gLang), sizeof(lang_value) - 1);
  odroid_dialog_choice_t options[] = {
      {300, curr_lang->s_LangUI, lang_value, 1, &lang_update_cb},
      ODROID_DIALOG_CHOICE_LAST,
  };

  while (true) {
    wdog_refresh();

    bool draw_frame = common_emu_frame_loop();
    uint32_t now = tamapoke_millis();

    /* The PAUSE menu lives in common_emu_input_loop(), and nothing here called
     * it -- so PAUSE did nothing at all: no menu, no volume, no brightness, no
     * exit. common_emu_frame_loop() above only paces the frame; it is a separate
     * contract and having one is not having the other. This is the same omission
     * Super Metroid shipped with, and the reason CLAUDE.md says to check the
     * caller rather than the callee. */
    odroid_gamepad_state_t joystick;
    odroid_input_read_gamepad(&joystick);
    common_emu_input_loop(&joystick, options, &repaint_ui);
    common_emu_input_loop_handle_turbo(&joystick);

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
