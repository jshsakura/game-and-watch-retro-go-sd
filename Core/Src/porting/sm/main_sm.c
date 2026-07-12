/* Super Metroid (snesrev/sm) — Game & Watch glue.
 *
 * Modelled on main_zelda3.c, with three things zelda3 does not need:
 *
 *  1. sm still reads the original ROM at runtime (RomPtr / g_rom), so the 3 MB
 *     image is cached into external flash and g_rom points straight at it. It is
 *     never copied into RAM — cart_load() would memcpy the whole thing, so the
 *     cart fields get filled in by hand instead.
 *  2. The per-frame driver lives in sm_cpu_infra.c, which exists only to run the
 *     reference SNES emulator alongside the reimplementation and diff them. The
 *     device has no emulator, so that file is not compiled and the two functions
 *     worth keeping (run a frame, pump the PPU) are reproduced here.
 *  3. The Korean patch replaces the game's Japanese text with Korean, and the
 *     switch is one word of WRAM. Expose it as a Language option, the way the
 *     zelda3 port does — the in-game OPTION MODE is not reachable from a
 *     Game & Watch d-pad without a lot of squinting.
 */

#include <odroid_system.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "main.h"
#include "gw_lcd.h"
#include "gw_linker.h"
#include "gw_buttons.h"
#include "gw_malloc.h"

#include "bq24072.h"
#include "stm32h7xx_hal.h"

#include "common.h"
#include "rom_manager.h"
#include "appid.h"
#include "rg_i18n.h"

#include "sm/src/snes/snes.h"
#include "sm/src/snes/ppu.h"
#include "sm/src/snes/cart.h"
#include "sm/src/sm_rtl.h"
#include "sm/src/spc_player.h"
#include "sm/src/types.h"
#include "sm/src/variables.h"

/* No -Ofast here: the overlay pool is full to the byte and the glue is not the
 * hot path — the game logic is, and it is built -Os for exactly this reason. */
#pragma GCC optimize("Os")

#define SM_AUDIO_SAMPLE_RATE   (16000)
#define FRAMERATE              (60)
#define SM_AUDIO_BUFFER_LENGTH (SM_AUDIO_SAMPLE_RATE / FRAMERATE)

#define SM_ROM_PATH  "/roms/homebrew/sm.smc"
#define SM_SRAM_SIZE (0x2000)
#define SM_CART_LOROM (1)

/* Globals that sm's SDL main.c owns upstream. */
bool g_debug_flag;
bool g_new_ppu = true;
bool g_other_image;
int g_got_mismatch_count;
SpcPlayer *g_spc_player;

void RtlApuLock(void) {}      /* single-threaded here: no audio thread to fence */
void RtlApuUnlock(void) {}

static int16_t audiobuffer_sm[SM_AUDIO_BUFFER_LENGTH];
static Snes *g_sm_snes;
static int g_input1_state;
static odroid_gamepad_state_t joystick;

/* Language: the Korean patch swaps the Japanese text path for Korean, so this
 * one WRAM word is the switch. 0 = the game's own default (English). */
static int selected_language_index = 0;
static char display_language_value[10];
static const char *const kSmLanguages[] = { "English", "Korean" };
#define SM_LANGUAGE_COUNT (sizeof(kSmLanguages) / sizeof(kSmLanguages[0]))

void NORETURN Die(const char *error) {
  printf("sm: %s\n", error);
  assert(0);
  while (1) {}
}

void Warning(const char *error) {
  printf("sm warning: %s\n", error);
}

/* ---------------------------------------------------------------- rodata ---
 * .rodata_sm is not linked into the firmware image: it lands in the fake RODATA
 * region, gets objcopy'd out to /roms/homebrew/sm.ro, and is cached into flash
 * at boot. Every pointer in the overlay that still refers to the link-time base
 * has to be rewritten to wherever the cache actually landed. Same trick, and the
 * same reason, as zelda3: 24 KB of rodata does not fit in the RAM budget.
 */
extern void *__rodata_sm_start__[];

static void PatchCodeRodataOffset(uint8 *rodata, uint32_t rodata_length) {
  uint32_t *ptr = (uint32_t *)__RAM_EMU_START__;
  uint32_t *end = (uint32_t *)&_OVERLAY_SM_BSS_END;

  uint32_t rodata_base = (uint32_t)__rodata_sm_start__;
  int32_t offset = (uint32_t)rodata - rodata_base;

  printf("sm rodata = %p base = 0x%08lX offset = 0x%08lX\n", rodata, rodata_base, offset);
  while (ptr < end) {
    if ((ptr < (uint32_t *)&_SM_MAIN_CODE_START) || (ptr > (uint32_t *)&_SM_MAIN_CODE_END)) {
      uint32_t value = *ptr;
      if ((value >= rodata_base) && (value < rodata_base + rodata_length)) {
        *ptr = value + offset;
        wdog_refresh();
      }
    }
    ptr++;
  }
}

/* ------------------------------------------------------------------ frame ---
 * sm_cpu_infra.c's RunOneFrameOfGame_Both() runs the emulator and the C
 * reimplementation over the same frame and compares them. On the device only
 * the reimplementation exists, which is exactly its RM_MINE branch.
 */
static void SmDrawFrameToPpu(void) {
  g_sm_snes->hPos = g_sm_snes->vPos = 0;
  while (!g_sm_snes->cpu->nmiWanted) {
    do {
      snes_handle_pos_stuff(g_sm_snes);
    } while (g_sm_snes->hPos != 0);
    if (g_sm_snes->vIrqEnabled && (g_sm_snes->vPos - 1) == g_sm_snes->vTimer)
      Vector_IRQ();
  }
  g_sm_snes->cpu->nmiWanted = false;
}

static void SmRunFrameDevice(uint16 input, int run_what) {
  g_sm_snes->input1->currentState = input;
  g_use_my_apu_code = true;
  g_sm_snes->runningWhichVersion = 0xff;
  RunOneFrameOfGame();
  SmDrawFrameToPpu();
  g_sm_snes->runningWhichVersion = 0;
}

/* ------------------------------------------------------------------ video --- */
static void DrawPpuFrame(uint16_t *framebuffer) {
  wdog_refresh();
  /* The PPU writes RGB565 straight into the LCD framebuffer — there is no room
   * for an intermediate buffer. 256 wide inside a 320-wide line, centred. */
  PpuBeginDrawing(g_sm_snes->ppu, (uint8_t *)(framebuffer + 32), GW_LCD_WIDTH * 2, 0);
}

/* ------------------------------------------------------------------ audio --- */
static void sm_sound_start(void) {
  memset(audiobuffer_sm, 0, sizeof(audiobuffer_sm));
  audio_start_playing(SM_AUDIO_BUFFER_LENGTH);
}

static void sm_sound_submit(void) {
  if (common_emu_sound_loop_is_muted())
    return;

  int16_t factor = common_emu_sound_get_volume();
  int16_t *sound_buffer = audio_get_active_buffer();
  uint16_t sound_buffer_length = audio_get_buffer_length();

  RtlRenderAudio(audiobuffer_sm, sound_buffer_length, 1);
  for (int i = 0; i < sound_buffer_length; i++)
    sound_buffer[i] = (audiobuffer_sm[i] * factor) >> 8;
}

/* ------------------------------------------------------------------- SRAM --- */
static void sm_system_SramSave(void) {
  char *path = odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ACTIVE_FILE->path);
  FILE *f = fopen(path, "wb");
  if (f != NULL) {
    fwrite(g_sram, 1, SM_SRAM_SIZE, f);
    fclose(f);
  }
  free(path);
}

/* ----------------------------------------------------------------- dialog --- */
static bool reset_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat) {
  if (event == ODROID_DIALOG_ENTER)
    RtlReset(0);
  return event == ODROID_DIALOG_ENTER;
}

static void sm_apply_language(void) {
  /* japanese_text_flag lives at g_ram + 0x9E2. With the Korean patch applied the
   * "Japanese" text path is Korean. It has to be held, not set once: the game
   * re-initialises it. */
  japanese_text_flag = (selected_language_index == 1) ? 1 : 0;
}

static bool update_language_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat) {
  int max_index = SM_LANGUAGE_COUNT - 1;

  if (event == ODROID_DIALOG_PREV)
    selected_language_index = selected_language_index > 0 ? selected_language_index - 1 : max_index;
  if (event == ODROID_DIALOG_NEXT)
    selected_language_index = selected_language_index < max_index ? selected_language_index + 1 : 0;

  strcpy(option->value, kSmLanguages[selected_language_index]);
  sm_apply_language();
  return event == ODROID_DIALOG_ENTER;
}

/* ------------------------------------------------------------------- main --- */
int app_main_sm(uint8_t load_state, uint8_t start_paused, int8_t save_slot) {
  printf("Super Metroid start\n");
  ram_start = (uint32_t)&_OVERLAY_SM_BSS_END;

  /* rodata out of RAM: cache it in flash and re-point the overlay at it. */
  uint32_t sm_rodata_length = 0;
  uint8 *sm_rodata = odroid_overlay_cache_file_in_flash("/roms/homebrew/sm.ro", &sm_rodata_length, false);
  if (sm_rodata == NULL)
    Die("Missing /roms/homebrew/sm.ro");
  PatchCodeRodataOffset(sm_rodata, sm_rodata_length);

  /* The game reads the original ROM the whole way through (RomPtr). Cache the
   * 3 MB image in external flash and XIP it — copying it into RAM is not an
   * option, which is why cart_load() is bypassed. */
  uint32_t rom_length = 0;
  uint8 *rom = odroid_overlay_cache_file_in_flash(SM_ROM_PATH, &rom_length, false);
  if (rom == NULL)
    Die("Missing " SM_ROM_PATH);

  common_emu_state.frame_time_10us = (uint16_t)(100000 / FRAMERATE + 0.5f);
  if (start_paused) {
    common_emu_state.pause_after_frames = 2;
    odroid_audio_mute(true);
  } else {
    common_emu_state.pause_after_frames = 0;
  }

  g_sm_snes = snes_init(g_ram);

  Cart *cart = g_sm_snes->cart;
  cart->type = SM_CART_LOROM;
  cart->rom = rom;                       /* XIP out of external flash */
  cart->romSize = (int)rom_length;
  cart->ram = (uint8_t *)ram_calloc(1, SM_SRAM_SIZE);
  cart->ramSize = SM_SRAM_SIZE;
  g_rom = cart->rom;
  g_sram = cart->ram;

  g_spc_player = SpcPlayer_Create();
  SpcPlayer_Initialize(g_spc_player);

  RtlSetupEmuCallbacks(NULL, &SmRunFrameDevice, NULL);
  RtlReset(0);

  /* Battery-backed save */
  char *sram_path = odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ACTIVE_FILE->path);
  FILE *file = fopen(sram_path, "rb");
  if (file != NULL) {
    fread(g_sram, 1, SM_SRAM_SIZE, file);
    fclose(file);
  }
  free(sram_path);

  strcpy(display_language_value, kSmLanguages[selected_language_index]);
  sm_apply_language();

  lcd_clear_buffers();
  lcd_wait_for_vblank();
  sm_sound_start();

  uint16_t *screen = lcd_get_active_buffer();

  while (true) {
    wdog_refresh();

    /* No in-game battery HUD: common_ingame_overlay() already draws one. */
    odroid_input_read_gamepad(&joystick);

    odroid_dialog_choice_t options[] = {
        {300, curr_lang->s_LangUI, display_language_value, 1, &update_language_cb},
        {300, curr_lang->s_Reset, NULL, 1, &reset_cb},
        ODROID_DIALOG_CHOICE_LAST};
    void _repaint(void) {
      screen = lcd_get_active_buffer();
      DrawPpuFrame(screen);
      common_ingame_overlay();
    }
    common_emu_input_loop(&joystick, options, &_repaint);

    /* SNES pad: B Y Select Start Up Down Left Right A X L R */
    bool pause_mod = joystick.values[ODROID_INPUT_VOLUME];
    bool game_mod = joystick.values[ODROID_INPUT_START];
    int in = 0;
    if (!pause_mod) {
      if (joystick.values[ODROID_INPUT_UP])    in |= 1 << 4;
      if (joystick.values[ODROID_INPUT_DOWN])  in |= 1 << 5;
      if (joystick.values[ODROID_INPUT_LEFT])  in |= 1 << 6;
      if (joystick.values[ODROID_INPUT_RIGHT]) in |= 1 << 7;
      if (!game_mod) {
        if (joystick.values[ODROID_INPUT_A])      in |= 1 << 8;   /* A: jump   */
        if (joystick.values[ODROID_INPUT_B])      in |= 1 << 0;   /* B: shoot  */
        if (joystick.values[ODROID_INPUT_SELECT]) in |= 1 << 9;   /* TIME:   X */
        if (joystick.values[ODROID_INPUT_Y])      in |= 1 << 1;   /* SELECT: Y */
        if (joystick.values[ODROID_INPUT_X])      in |= 1 << 3;   /* START     */
      } else {
        if (joystick.values[ODROID_INPUT_B])      in |= 1 << 10;  /* GAME+B: L */
        if (joystick.values[ODROID_INPUT_A])      in |= 1 << 11;  /* GAME+A: R */
        if (joystick.values[ODROID_INPUT_SELECT]) in |= 1 << 2;   /* GAME+TIME: Select */
      }
    }
    g_input1_state = in;

    sm_apply_language();   /* the game clears it on init, so hold it */

    screen = lcd_get_active_buffer();
    DrawPpuFrame(screen);

    RtlRunFrame(g_input1_state);

    common_ingame_overlay();
    sm_sound_submit();

    lcd_swap();

    common_emu_sound_sync(false);
  }

  return 0;
}

/* ============================================================================
 * MEASUREMENT STUBS — NOT SHIPPABLE. These exist only so the image links and we
 * can read the RAM_EMU budget off the map file. Two real problems hide behind
 * them, both discovered while wiring this up:
 *
 *  - Call() runs original 65816 ROM code through the CPU emulator. Bang_Main()
 *    (sm_a3.c) does it on a live gameplay path, so sm's reimplementation is NOT
 *    complete — some enemy routines are still ROM asm. RunAsmCode() and the rest
 *    of sm_cpu_infra.c have to come along, minus its three ~200 KB Snapshot
 *    globals (the emulator-compare machinery), which would blow the budget.
 *
 *  - RtlUpdateSnesPatchForBugfix() PatchBytes()es the ROM at runtime. Our ROM is
 *    XIP out of read-only external flash. The three patch sites are ~4.5 KB apart
 *    in bank $85, so the fix is probably to pre-patch the .smc when it is pushed
 *    to the SD card and make PatchBytes a no-op here.
 * ========================================================================== */
void Call(uint32 addr) { (void)addr; }
void DebugGameOverMenu(void) {}
void RtlUpdateSnesPatchForBugfix(void) {}
uint16 currently_installed_bug_fix_counter;
void apu_reset(Apu *apu) { (void)apu; }   /* snes->apu is NULL here: spc_player does the audio */
