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
#include "gw_flash.h"
#include "gw_flash_alloc.h"

#include "bq24072.h"
#include "stm32h7xx_hal.h"

#include "common.h"
#include "rom_manager.h"
#include "appid.h"
#include "rg_i18n.h"

#include "sm/src/snes/snes.h"
#include "sm/src/snes/ppu.h"
#include "sm/src/snes/cart.h"
#include "sm/src/snes/apu.h"
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

/* ...and the ones sm_cpu_infra.c owned. That file is not compiled here (it is the
 * emulator-comparison harness), and dropping it left these referenced but never
 * defined. The linker does not complain about that across overlays: it quietly
 * bound them to Super Mario World's identically-named globals, whose addresses lie
 * inside OUR overlay's bss once we are loaded. So sm was driving the SNES bus
 * through a pointer that was really some unrelated variable of its own — it died
 * on the first register read.
 *
 * They are in sm_redefines now, so they are private to this overlay and a missing
 * one is a link error instead of a silent cross-core alias. Keep it that way. */
Snes *g_snes;                 /* THE bus pointer: sm_rtl.c's WriteReg/ReadReg use it */
bool g_use_my_apu_code = true;
bool g_fail;

void RtlApuLock(void) {}      /* single-threaded here: no audio thread to fence */
void RtlApuUnlock(void) {}

static int16_t audiobuffer_sm[SM_AUDIO_BUFFER_LENGTH];
static Snes *g_sm_snes;
static int g_input1_state;
static odroid_gamepad_state_t joystick;

/* Language. japanese_text_flag is one WRAM word, and it selects between the two
 * languages the ROM itself carries: English, and whatever the second one is. On a
 * stock Japanese ROM that is Japanese; with the Korean fan patch applied it is
 * Korean. So name the second option after the ROM that is actually loaded rather
 * than promising Korean to someone who supplied a stock image — the switch works
 * either way, but the label should not lie.
 *
 * Detection: the patch redirects the intro's text tables, and the operand it
 * rewrites is the one thing that is guaranteed to differ. See IntroTextTable() in
 * external/sm's sm_8b.c, which follows the same redirect. */
#define SM_LANG_SITE      0x8BB5A3   /* the LDY operand for the first intro page */
#define SM_LANG_SITE_STOCK 0xD085    /* what a stock ROM names there */

static int selected_language_index = 0;
static char display_language_value[10];
static bool rom_is_patched = false;
static const char *SmLanguageName(int index) {
  if (index == 0)
    return "English";
  return rom_is_patched ? "Korean" : "Japanese";
}
#define SM_LANGUAGE_COUNT 2

void NORETURN Die(const char *error) {
  printf("sm: %s\n", error);
  assert(0);
  while (1) {}
}

void Warning(const char *error) {
  printf("sm warning: %s\n", error);
}



/* ------------------------------------------------------------------- XIP ---
 * The overlay pool is 724 KB and the game wants more, so the cold banks
 * (cinematics and friends) plus ALL of the game's rodata are linked at a
 * sentinel address, shipped as one file — /roms/homebrew/sm.xip — cached into
 * QSPI flash, and executed and read straight out of it. Same trick as PICO-8
 * (Pico8CacheCodeToFlash in rg_emulators.c).
 *
 * Code and rodata share the region deliberately. The cold banks hold ~90
 * pointers into rodata; if the two were separate cache entries, the copy of the
 * code in flash would have another entry's address baked into it, and the pair
 * would go stale the moment the circular cache evicted one and not the other.
 * As one blob every such pointer is a sentinel into the blob itself, so one
 * relocation against one base fixes all of them, and it stays fixed for as long
 * as the entry lives.
 *
 * The relocation happens on the way in — the cache hands each buffer to
 * SmRelocateXip() before programming it — rather than by rewriting flash that is
 * already written. A rewrite has to erase first, and an erase interrupted by a
 * flat battery leaves a blank hole indistinguishable from a finished job. It
 * would also have to erase in 4 KB pieces, which the 256 KB-sector chips cannot
 * do at all. On a cache hit nothing is written and nothing needs to be: the copy
 * in flash was relocated to that same address when it was first stored.
 *
 * Both call directions across the split work out (verified with nm on the
 * veneers):
 *   RAM -> XIP: the veneer sits in the overlay, its literal is a sentinel, and
 *               the overlay scan in app_main_sm() rewrites it.
 *   XIP -> RAM: the veneer sits in the blob, its literal is the overlay's VMA,
 *               which IS the runtime address — already correct.
 */
#define SM_CODE_BASE  0xDEAD0000u
#define SM_XIP_PATH   "/roms/homebrew/sm.xip"

static uint8_t *g_xip_addr;
static uint32_t g_xip_size;
static int32_t  g_xip_offset;

/* Rewrite every sentinel-range word in [start, end) to where the blob really
 * landed. Thumb bit included, hence the & ~1. */
static int PatchSmSentinels(uint32_t *start, uint32_t *end, int32_t offset, uint32_t size) {
  int patched = 0;
  for (uint32_t *p = start; p < end; p++) {
    uint32_t v = *p;
    if ((v & ~1u) >= SM_CODE_BASE && (v & ~1u) < SM_CODE_BASE + size) {
      *p = (uint32_t)(v + offset);
      patched++;
    }
  }
  return patched;
}

/* Relocation hook: runs on each buffer of sm.xip on its way into the flash. */
static void SmRelocateXip(uint8_t *buffer, uint32_t length, uint32_t offset_in_file,
                          uint8_t *file_address, uint32_t file_size) {
  (void)offset_in_file;
  int32_t offset = (int32_t)((uint32_t)file_address - SM_CODE_BASE);
  PatchSmSentinels((uint32_t *)buffer, (uint32_t *)(buffer + (length & ~3u)), offset, file_size);
}

static bool SmCacheXipToFlash(void) {
  g_xip_size = 0;
  g_xip_addr = odroid_overlay_cache_file_in_flash_relocate(SM_XIP_PATH, &g_xip_size, false,
                                                           &SmRelocateXip);
  if (g_xip_addr == NULL || g_xip_size == 0) {
    printf("sm: %s missing\n", SM_XIP_PATH);
    return false;
  }
  g_xip_offset = (int32_t)((uint32_t)g_xip_addr - SM_CODE_BASE);
  printf("sm: xip blob at %p, %lu bytes, offset 0x%08lX\n",
         g_xip_addr, (unsigned long)g_xip_size, (unsigned long)g_xip_offset);
  return true;
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

  strcpy(option->value, SmLanguageName(selected_language_index));
  sm_apply_language();
  return event == ODROID_DIALOG_ENTER;
}

/* ------------------------------------------------------------------- main --- */
int app_main_sm(uint8_t load_state, uint8_t start_paused, int8_t save_slot) {
  printf("Super Metroid start\n");
  ram_start = (uint32_t)&_OVERLAY_SM_BSS_END;

  /* Sets the SAI up for OUR sample rate, and loads the saved volume. Without it
   * the audio hardware keeps whatever rate the previous app left behind and the
   * game plays at the wrong pitch. zelda3 and smw both do this; sm did not. */
  odroid_system_init(APPID_SM, SM_AUDIO_SAMPLE_RATE);

  /* The game reads the original ROM the whole way through (RomPtr). Cache the
   * 3 MB image in external flash and XIP it — copying it into RAM is not an
   * option, which is why cart_load() is bypassed.
   *
   * The ROM goes in FIRST, and the order matters. The flash cache is circular:
   * a file that does not fit at the write pointer rewinds it to the base and
   * overwrites whatever is there. Caching the ROM after the code blob could
   * therefore erase the blob we are about to execute from — with the pointer to
   * it already in hand. Nothing is written after the blob, so nothing can. */
  uint32_t rom_length = 0;
  uint8 *rom = odroid_overlay_cache_file_in_flash(SM_ROM_PATH, &rom_length, false);
  if (rom == NULL)
    Die("Missing " SM_ROM_PATH);

  /* Which ROM is this? LoROM: bank $8B lives at file offset 0x0B * 0x8000. */
  {
    uint32_t off = (uint32_t)((SM_LANG_SITE >> 16 & 0x7F) * 0x8000 + (SM_LANG_SITE & 0x7FFF));
    if (off + 1 < rom_length) {
      uint16_t operand = (uint16_t)(rom[off] | (rom[off + 1] << 8));
      rom_is_patched = (operand != SM_LANG_SITE_STOCK);
    }
    printf("sm: rom is %s\n", rom_is_patched ? "fan-patched (2nd language = Korean)"
                                             : "stock (2nd language = Japanese)");
  }

  /* The cold banks and all of the game's rodata live in QSPI flash — neither
   * fits in the overlay pool. */
  if (!SmCacheXipToFlash())
    Die("Missing " SM_XIP_PATH);

  /* Everything in the overlay that points into the blob — the RAM->XIP call
   * veneers, and every reference to the game's rodata — still holds a sentinel.
   *
   * main_sm.o is skipped: SM_CODE_BASE is a literal in this file's own code, and
   * a scan that could not tell it from a reference would rewrite the constant the
   * scan is built on. That is also why the glue's rodata is kept in RAM (see the
   * linker script) — its string literals have to be readable before this runs. */
  {
    int n = PatchSmSentinels((uint32_t *)__RAM_EMU_START__, (uint32_t *)&_SM_MAIN_CODE_START,
                             g_xip_offset, g_xip_size)
          + PatchSmSentinels((uint32_t *)&_SM_MAIN_CODE_END, (uint32_t *)&_OVERLAY_SM_BSS_START,
                             g_xip_offset, g_xip_size);
    printf("sm: patched %d sentinel refs in the overlay\n", n);
    wdog_refresh();
  }

  common_emu_state.frame_time_10us = (uint16_t)(100000 / FRAMERATE + 0.5f);
  if (start_paused) {
    common_emu_state.pause_after_frames = 2;
    odroid_audio_mute(true);
  } else {
    common_emu_state.pause_after_frames = 0;
  }

  g_sm_snes = snes_init(g_ram);
  g_snes = g_sm_snes;   /* sm_rtl.c drives every hardware register through g_snes.
                         * Nothing was setting it: sm_cpu_infra.c used to, and that
                         * file is gone. Without this the whole SNES bus runs off a
                         * garbage pointer. */

  Cart *cart = g_sm_snes->cart;
  cart->type = SM_CART_LOROM;
  cart->rom = rom;                       /* XIP out of external flash */
  cart_setRomSize(cart, (int)rom_length); /* NOT cart->romSize = ...: the mirroring
                                           * fast-path mask has to be derived with it */
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

  strcpy(display_language_value, SmLanguageName(selected_language_index));
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
 * sm_cpu_infra.c is not compiled in. It is the harness that runs the reference
 * SNES emulator next to the reimplementation and diffs them, and it drags in the
 * 65816 interpreter plus three ~200 KB Snapshot globals. The handful of symbols
 * the game itself still needs from it are defined here instead.
 *
 * Nothing on the device executes 65816 code: the ROM is a read-only data source
 * (RomPtr) and every routine runs as C. --gc-sections drops cpu_runOpcode()
 * accordingly. That makes the two interesting stubs below correct, not merely
 * convenient — each one was checked against the ROM:
 *
 *  - Call() runs ROM asm through the interpreter. Its only live caller is
 *    Bang_Main() (sm_a3.c), which tail-jumps into bank $A3 through a per-instance
 *    function pointer (LDX cur_enemy_index; JMP ($0FB4,X)) that sm's decompiler
 *    could not resolve. The Bang enemy ($A0:DB40) is unused in the retail ROM:
 *    walking all 259 room headers and their 297 enemy-population lists turns up
 *    140 distinct enemies and Bang is not one of them; its id appears nowhere in
 *    bank $A1 (populations) or bank $B4 (enemy sets, without which its graphics
 *    are never even loaded); and no C code hardcodes the id. It cannot spawn, so
 *    this cannot be reached. If a ROM hack ever did spawn one, the enemy would
 *    stand still rather than fault.
 *
 *  - RtlUpdateSnesPatchForBugfix() rewrites ROM bytes so the interpreter takes a
 *    fixed path through HandleMessageBoxInteraction. With no interpreter running,
 *    those bytes are never executed; they sit in bank $85 code that the C side
 *    never reads as data. (Independently confirmed on the host harness: skipping
 *    the patch left 100 sampled frames of a 6,000-frame run byte-identical.)
 *    It could not be honoured anyway — our ROM is XIP out of read-only flash.
 * ========================================================================== */
void Call(uint32 addr) { (void)addr; }
void DebugGameOverMenu(void) {}
void RtlUpdateSnesPatchForBugfix(void) {}
uint16 currently_installed_bug_fix_counter;

/* snes->apu is NULL here — spc_player makes the sound, and the 65816 interpreter
 * is gc'd out, so none of these can run. They exist because snes.c and cpu.c still
 * name them, and a name with no definition is not an error across overlays: it
 * silently aliases another core's. Better a stub that cannot be reached than a
 * jump into Super Mario World's address space. */
void apu_reset(Apu *apu) { (void)apu; }
void apu_cycle(Apu *apu) { (void)apu; }
void apu_free(Apu *apu) { (void)apu; }
void apu_saveload(Apu *apu, SaveLoadFunc *func, void *ctx) { (void)apu; (void)func; (void)ctx; }
void ppu_copy(Ppu *ppu, Ppu *ppu_src) { (void)ppu; (void)ppu_src; }
int  CpuOpcodeHook(uint32 addr) { (void)addr; return 0; }
bool HookedFunctionRts(int is_long) { (void)is_long; return false; }
