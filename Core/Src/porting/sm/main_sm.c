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
#include "gw_ofw.h"
#include "gw_sleep.h"
#include "error_screens.h"

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
#include "sm_state_header.h"

/* No -Ofast here: the overlay pool is full to the byte and the glue is not the
 * hot path — the game logic is, and it is built -Os for exactly this reason. */
#pragma GCC optimize("Os")

#define SM_AUDIO_SAMPLE_RATE   (16000)
#define FRAMERATE              (60)
#define SM_AUDIO_BUFFER_LENGTH (SM_AUDIO_SAMPLE_RATE / FRAMERATE)

#define SM_ROM_PATH  "/roms/homebrew/sm.smc"
#define SM_SRAM_SIZE (0x2000)
#define SM_CART_LOROM (1)

/* The port indexes the image as a flat LoROM (bank N at N * 0x8000), so the
 * 512-byte copier header half the dumps in circulation carry would put every
 * read 512 bytes off. It is unambiguous — nothing else is 3 MB + 512 — so skip
 * it rather than refuse the file. Any other size is not this game. */
#define SM_ROM_SIZE      (3u * 1024 * 1024)
#define SM_COPIER_HEADER (512u)

/* How long the fatal screen stays up before the console sleeps, in seconds. */
#define SM_FATAL_SLEEP_S (600)

/* Declared in headers we do not pull in whole: funcs.h drags the game's entire
 * symbol table, and sm_cpu_infra.h belongs to the emulator-comparison harness.
 * Both were being called with no prototype at all, which the sm build used to
 * hide behind -Wno-implicit-function-declaration. */
void RunOneFrameOfGame(void);
void Vector_IRQ(void);

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

/* A missing or wrong file has to say so on the LCD. Die() printf()s down a UART
 * nobody has and then assert()s, which puts a fault screen up with a file and a
 * line number on it — true, and of no use whatever to the person holding the
 * console. Say the name of the file, and what to do about it, the way
 * sdcard_error_screen() already does. */
static void NORETURN SmFatal(const char *line_1, const char *line_2) {
  printf("sm: FATAL %s / %s\n", line_1, line_2 ? line_2 : "");
  lcd_backlight_set(180);
  draw_error_screen("SUPER METROID", line_1, line_2);

  /* Hold the screen. Do NOT sleep: GW_EnterDeepSleep() came back through
   * gw_sleep.c, whose sdcard_init() failed, and the last thing the player saw
   * was "No SD CARD found" — a message about a card that was never the problem.
   * A fatal screen has one job, which is to still be there when it is read. */
  while (1) {
    wdog_refresh();
    lcd_sync();
    lcd_swap();
    HAL_Delay(20);
  }
}

void NORETURN Die(const char *error) {
  SmFatal(error, NULL);
}

/* Is the ROM in flash still the ROM on the card?
 *
 * A new game is a black screen while a save loads and plays, and the launch after
 * that is fine. The suspicion is the flash cache: it is a ring, and the write that
 * caches the code blob can come round onto the ROM whose pointer we are already
 * holding. The port executes no 65816 — it only READS the ROM — so a hole takes out
 * only the scenes that read from it, and the intro's text tables (bank $8B) are
 * read by nothing else.
 *
 * That is a story, not a fact, and two attempts to fix it before proving it cost
 * two broken builds. So prove it, or kill it. This writes nothing and erases
 * nothing: it samples 64 bytes every 64 KB — no smaller than an erase block, so no
 * hole can hide between samples — and compares flash against the card. ~3 KB of SD
 * reads. If they agree, the cache is innocent and the bug is in the intro. */
#define SM_VERIFY_STRIDE (64u * 1024)
#define SM_VERIFY_BYTES  (64u)

static void SmVerifyRomAgainstCard(const uint8 *rom, uint32_t rom_length,
                                   uint32_t header_bytes) {
  FILE *f = fopen(SM_ROM_PATH, "rb");
  if (f == NULL)
    return;                       /* it launched, so it is there; nothing to prove */

  uint8_t from_card[SM_VERIFY_BYTES];
  for (uint32_t off = 0; off + SM_VERIFY_BYTES <= rom_length; off += SM_VERIFY_STRIDE) {
    if (fseek(f, (long)(off + header_bytes), SEEK_SET) != 0)
      break;
    if (fread(from_card, 1, SM_VERIFY_BYTES, f) != SM_VERIFY_BYTES)
      break;

    if (memcmp(from_card, rom + off, SM_VERIFY_BYTES) != 0) {
      fclose(f);
      char where[40];
      snprintf(where, sizeof(where), "ROM cache damaged at 0x%06lX", (unsigned long)off);
      SmFatal(where, "Flash copy differs from the SD file");
    }
  }
  fclose(f);
  printf("sm: rom in flash matches the card\n");
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
    /* A line at a time. Walking the dot clock two dots at a time — which is what
     * snes_handle_pos_stuff() does, and what this loop used to call 178,684 times
     * a frame — burns most of those calls on a counter increment and six branches
     * that never fire. */
    snes_run_line(g_sm_snes);
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

/* ------------------------------------------------------------------ video ---
 * The SNES is 256x224 and the screen is 320x240, and there is no room for a
 * staging buffer to scale through — which is why this core used to ignore the
 * launcher's scaling setting entirely and just centre the native image.
 *
 * So scale a line at a time. The PPU renders into a 256-pixel line buffer (512
 * bytes, in cached RAM) and hands each finished line here, and this places it:
 *
 *   OFF   256x224, centred. Native, no filtering, black border.
 *   FIT   256x240 — full height, black pillars. The 224->240 stretch duplicates
 *         one line in every 14, which is what the sibling SNES ports do.
 *   FULL  320x240 — fills the screen. 256->320 duplicates one pixel in every 4.
 *
 * Nearest-neighbour, because anything else costs a multiply per pixel and we are
 * at 68% of the frame budget as it is. */
#define SM_W 256
#define SM_H 224

static uint16_t sm_line[SM_W];
static uint16_t *sm_fb;                       /* the framebuffer this frame lands in */
static odroid_display_scaling_t sm_scaling = ODROID_DISPLAY_SCALING_COUNT;
static uint16_t sm_xmap[GW_LCD_WIDTH];        /* dst x -> src x, for FULL */

static void sm_blit_line(unsigned y, const uint16_t *line) {
  if (y < 1 || y > SM_H || !sm_fb)
    return;
  unsigned s = y - 1;                          /* source row, 0-based */

  switch (sm_scaling) {
    case ODROID_DISPLAY_SCALING_OFF: {
      uint16_t *dst = sm_fb + (unsigned)((240 - SM_H) / 2 + s) * GW_LCD_WIDTH + (GW_LCD_WIDTH - SM_W) / 2;
      memcpy(dst, line, SM_W * sizeof(uint16_t));
      break;
    }
    case ODROID_DISPLAY_SCALING_FULL: {
      /* one source row covers one or two destination rows */
      unsigned d0 = s * 240 / SM_H, d1 = (s + 1) * 240 / SM_H;
      for (unsigned d = d0; d < d1; d++) {
        uint16_t *dst = sm_fb + d * GW_LCD_WIDTH;
        for (unsigned x = 0; x < GW_LCD_WIDTH; x++)
          dst[x] = line[sm_xmap[x]];
      }
      break;
    }
    default: {                                 /* FIT and CUSTOM: full height, native width */
      unsigned d0 = s * 240 / SM_H, d1 = (s + 1) * 240 / SM_H;
      for (unsigned d = d0; d < d1; d++)
        memcpy(sm_fb + d * GW_LCD_WIDTH + (GW_LCD_WIDTH - SM_W) / 2, line, SM_W * sizeof(uint16_t));
      break;
    }
  }
}

static void DrawPpuFrame(uint16_t *framebuffer) {
  wdog_refresh();

  odroid_display_scaling_t scaling = odroid_display_get_scaling_mode();
  if (scaling != sm_scaling) {
    sm_scaling = scaling;
    for (unsigned x = 0; x < GW_LCD_WIDTH; x++)
      sm_xmap[x] = (uint16_t)(x * SM_W / GW_LCD_WIDTH);
    /* The borders belong to whatever was on screen before; both buffers need it. */
    lcd_clear_buffers();
  }

  sm_fb = framebuffer;
  g_ppu_line_cb = &sm_blit_line;
  PpuBeginDrawing(g_sm_snes->ppu, (uint8_t *)sm_line, 0, 0);  /* pitch 0: every line here */
}

/* ------------------------------------------------------------------ audio --- */
static void sm_sound_start(void) {
  memset(audiobuffer_sm, 0, sizeof(audiobuffer_sm));
  audio_start_playing(SM_AUDIO_BUFFER_LENGTH);
}

/* Every emulated frame, drawn or skipped: this is what advances the SPC player.
 * Tie it to the drawn frames and the music slows down with the video. */
static void sm_audio_render(void) {
  RtlRenderAudio(audiobuffer_sm, SM_AUDIO_BUFFER_LENGTH, 1);
}

/* Drawn frames only: hand the frame's samples to the DMA half that is free. */
static void sm_sound_submit(void) {
  if (common_emu_sound_loop_is_muted())
    return;

  int16_t factor = common_emu_sound_get_volume();
  int16_t *sound_buffer = audio_get_active_buffer();
  uint16_t n = audio_get_buffer_length();
  if (n > SM_AUDIO_BUFFER_LENGTH)
    n = SM_AUDIO_BUFFER_LENGTH;

  for (int i = 0; i < n; i++)
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

/* -------------------------------------------------------------- savestate --- */
/* RtlSaveLoadState streams the state through these; nothing is staged in RAM,
 * because there is no RAM to stage ~270 KB in.
 *
 * Header magic/version/struct and the validation logic live in
 * sm_state_header.h, not here — that is what lets a host test exercise the
 * "refuse a foreign or truncated file" behaviour without linking the SNES
 * core this file drives. See that header for why the header exists at all. */

static FILE *savestate_file;
static size_t savestate_bytes;
static bool savestate_short;

/* How many bytes THIS build's savestate is, measured rather than assumed: with
 * savestate_file NULL, sm_state_write() writes nothing and only counts. Taken
 * once at boot, before the game has anything to lose. See sm_state_header.h for
 * why the length, and not SM_STATE_VERSION, is what a load is checked against. */
static uint32_t savestate_expected_bytes;

/* Set while loading a file from the older layout, whose last four bytes (the tail
 * of the SPC player's state) simply are not there. sm_state_read() takes them as
 * zero, and that is not a truncation — it is the only difference between the two
 * layouts, and it is in the last segment. */
static bool savestate_allow_short_tail;

static void sm_state_write(void *ctx, void *data, size_t size) {
  (void)ctx;
  wdog_refresh();
  if (savestate_file)
    fwrite(data, 1, size, savestate_file);
  savestate_bytes += size;
}

static void sm_state_read(void *ctx, void *data, size_t size) {
  (void)ctx;
  wdog_refresh();
  size_t got = savestate_file ? fread(data, 1, size, savestate_file) : 0;
  savestate_bytes += got;
  if (got == size)
    return;

  /* Short. On a legacy file this is expected exactly once, at the very end, and
   * zero is the right value; anywhere else it means the file lied about its own
   * length, which sm_state_load_verdict() should already have refused. */
  memset((uint8_t *)data + got, 0, size - got);
  if (!savestate_allow_short_tail)
    savestate_short = true;
}

static bool sm_system_SaveState(char *pathName) {
  odroid_audio_mute(true);
  savestate_file = fopen(pathName, "wb");
  bool ok = savestate_file != NULL;
  if (ok) {
    sm_state_header_t hdr = { SM_STATE_MAGIC, SM_STATE_VERSION, 0 };
    fwrite(&hdr, 1, sizeof(hdr), savestate_file);   /* patched below */

    savestate_bytes = 0;
    RtlSaveLoadState(kSaveLoad_Save, &sm_state_write, NULL);

    hdr.bytes = (uint32_t)savestate_bytes;
    fseek(savestate_file, 0, SEEK_SET);
    fwrite(&hdr, 1, sizeof(hdr), savestate_file);
    fclose(savestate_file);
    savestate_file = NULL;
    printf("sm: saved %lu bytes\n", (unsigned long)hdr.bytes);
  } else {
    printf("sm: savestate fopen failed: %s\n", pathName);
  }
  odroid_audio_mute(false);
  return ok;
}

static bool sm_system_LoadState(char *pathName) {
  odroid_audio_mute(true);
  savestate_file = fopen(pathName, "rb");
  if (savestate_file == NULL) {
    odroid_audio_mute(false);
    return false;
  }

  sm_state_header_t hdr = { 0, 0, 0 };
  bool ok = sm_state_header_read(savestate_file, &hdr);
  if (!ok) {
    /* Written by a different build. Loading it would restore a struct dump into
     * structs that have since moved: the game does not crash, it just renders
     * black, and nothing says why. Leave the running game alone. */
    printf("sm: savestate is not this build's (magic=%08lx version=%lu) — refusing\n",
           (unsigned long)hdr.magic, (unsigned long)hdr.version);
    fclose(savestate_file);
    savestate_file = NULL;
    odroid_audio_mute(false);
    return false;
  }

  /* Decide BEFORE streaming a byte. RtlSaveLoadState reads straight into g_ram,
   * VRAM and the live Ppu — there is nowhere to stage 270 KB — so by the time a
   * mid-stream read runs out of file, the game it was going to fall back to is
   * already half-overwritten. Everything the verdict needs is knowable now: the
   * header, the file's size, and what this build itself streams. */
  long here = ftell(savestate_file);
  fseek(savestate_file, 0, SEEK_END);
  long avail = ftell(savestate_file) - here;
  fseek(savestate_file, here, SEEK_SET);

  sm_state_load_t verdict =
      sm_state_load_verdict(hdr, savestate_expected_bytes, avail);

  if (verdict == SM_STATE_LOAD_REFUSE) {
    printf("sm: savestate says %lu bytes, this build writes %lu, file holds %ld — "
           "a different layout or a truncated file; refusing before it touches the game\n",
           (unsigned long)hdr.bytes, (unsigned long)savestate_expected_bytes, avail);
    fclose(savestate_file);
    savestate_file = NULL;
    odroid_audio_mute(false);
    return false;
  }

  savestate_allow_short_tail = (verdict == SM_STATE_LOAD_SHORT_TAIL);
  if (savestate_allow_short_tail)
    printf("sm: savestate is the older layout (%lu bytes) — the music player's last "
           "4 bytes are absent; loading it as zeroes\n", (unsigned long)hdr.bytes);

  savestate_bytes = 0;
  savestate_short = false;
  RtlSaveLoadState(kSaveLoad_Load, &sm_state_read, NULL);
  fclose(savestate_file);
  savestate_file = NULL;
  savestate_allow_short_tail = false;

  if (savestate_short) {
    printf("sm: savestate ran short mid-stream (%lu of %lu bytes)\n",
           (unsigned long)savestate_bytes, (unsigned long)hdr.bytes);
    ok = false;
  }
  odroid_audio_mute(false);
  return ok;
}

/* The PPU renders line by line while the frame runs, so there is no "draw the
 * current frame again" call to make here — re-rendering would mean running the
 * game another frame. Hand back the buffer the last frame actually landed in. */
static uint16_t *sm_last_frame;

static void *sm_system_Screenshot(void) {
  return sm_last_frame ? sm_last_frame : lcd_get_active_buffer();
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
  /* Save/load state, screenshot and the SRAM autosave are all wired through
   * here. sm never called it, so PAUSE+A / PAUSE+B did nothing at all and the
   * SRAM was only ever written by the code path nothing called. */
  odroid_system_emu_init(&sm_system_LoadState, &sm_system_SaveState,
                         &sm_system_Screenshot, NULL, NULL, &sm_system_SramSave);

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
  uint32_t rom_header_bytes = 0;
  uint8 *rom = odroid_overlay_cache_file_in_flash(SM_ROM_PATH, &rom_length, false);
  if (rom == NULL)
    SmFatal("Missing " SM_ROM_PATH, "Copy your ROM there - see README");

  /* Nothing checked this before, and a wrong file did not fail: it was read as
   * if it were the game, and the console died with no idea why. */
  if (rom_length == SM_ROM_SIZE + SM_COPIER_HEADER) {
    printf("sm: sm.smc carries a 512-byte copier header; skipping it\n");
    rom += SM_COPIER_HEADER;
    rom_length -= SM_COPIER_HEADER;
    rom_header_bytes = SM_COPIER_HEADER;
  } else if (rom_length != SM_ROM_SIZE) {
    SmFatal("sm.smc is not a 3 MB Super Metroid ROM", "Run tools/sm_prepare_rom.py on it");
  }

  /* Which ROM is this? LoROM: bank $8B lives at file offset 0x0B * 0x8000. */
  {
    uint32_t off = (uint32_t)((SM_LANG_SITE >> 16 & 0x7F) * 0x8000 + (SM_LANG_SITE & 0x7FFF));
    if (off + 1 < rom_length) {
      uint16_t operand = (uint16_t)(rom[off] | (rom[off + 1] << 8));
      rom_is_patched = (operand != SM_LANG_SITE_STOCK);
    }
    printf("sm: rom is %s\n", rom_is_patched ? "fan-patched (2nd language = Korean)"
                                             : "stock (2nd language = Japanese)");
    /* Someone who flashed the Korean patch onto the cart wants the Korean text —
     * the title screen is already in it. Default to the ROM's second language,
     * and leave the toggle for going back to English. */
    selected_language_index = rom_is_patched ? 1 : 0;
  }

  /* The cold banks and all of the game's rodata live in QSPI flash — neither
   * fits in the overlay pool. */
  if (!SmCacheXipToFlash())
    SmFatal("Missing " SM_XIP_PATH, "Re-run the retro-go_update.bin update");

  /* The blob's write is the suspect, so ask right after it: does the ROM in flash
   * still match the card? Reads only. */
  SmVerifyRomAgainstCard(rom, rom_length, rom_header_bytes);

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

  /* Count what a save of THIS build streams. savestate_file is NULL, so
   * sm_state_write() writes nothing and only adds up the sizes. Done here, on a
   * freshly reset core, where there is no game state to disturb — and before any
   * load, which is the thing that needs the number. */
  savestate_file = NULL;
  savestate_bytes = 0;
  RtlSaveLoadState(kSaveLoad_Save, &sm_state_write, NULL);
  savestate_expected_bytes = (uint32_t)savestate_bytes;
  printf("sm: this build's savestate is %lu bytes\n",
         (unsigned long)savestate_expected_bytes);

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

  if (load_state)
    odroid_system_emu_load_state(save_slot);
  else
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

    /* SNES pad: B Y Select Start Up Down Left Right A X L R
     *
     * ODROID_INPUT_Y and _X are the shell's SELECT and START keys, and those two
     * are wired on Zelda units only (main.h: "Zelda only buttons; they are not
     * connected on mario"). Bound to them directly, SNES Y (item cancel — how you
     * fire a missile) and SNES Start (the map screen) were unpressable on a Mario
     * unit, which is the default target. zelda3 solves this by branching on
     * get_ofw_is_mario(); do the same and give those two a home on GAME + UP/DOWN.
     *
     * A direction has to step aside for that: with GAME held, UP/DOWN are the two
     * buttons, so they must not also reach the game as a direction, or GAME+UP
     * would fire the missile *while aiming up* instead of ahead. LEFT/RIGHT stay
     * live — aiming diagonally with L/R needs them. */
    bool pause_mod = joystick.values[ODROID_INPUT_VOLUME];
    bool game_mod = joystick.values[ODROID_INPUT_START];
    bool mario_btn_mod = game_mod && get_ofw_is_mario();
    int in = 0;
    if (!pause_mod) {
      if (joystick.values[ODROID_INPUT_UP]   && !mario_btn_mod) in |= 1 << 4;
      if (joystick.values[ODROID_INPUT_DOWN] && !mario_btn_mod) in |= 1 << 5;
      if (joystick.values[ODROID_INPUT_LEFT])  in |= 1 << 6;
      if (joystick.values[ODROID_INPUT_RIGHT]) in |= 1 << 7;
      if (!game_mod) {
        if (joystick.values[ODROID_INPUT_A])      in |= 1 << 8;   /* A: jump   */
        if (joystick.values[ODROID_INPUT_B])      in |= 1 << 0;   /* B: dash   */
        if (joystick.values[ODROID_INPUT_SELECT]) in |= 1 << 9;   /* TIME:   X */
        if (joystick.values[ODROID_INPUT_Y])      in |= 1 << 1;   /* SELECT: Y (Zelda unit) */
        if (joystick.values[ODROID_INPUT_X])      in |= 1 << 3;   /* START     (Zelda unit) */
      } else {
        if (joystick.values[ODROID_INPUT_B])      in |= 1 << 10;  /* GAME+B: L */
        if (joystick.values[ODROID_INPUT_A])      in |= 1 << 11;  /* GAME+A: R */
        if (joystick.values[ODROID_INPUT_SELECT]) in |= 1 << 2;   /* GAME+TIME: Select */
        if (mario_btn_mod) {
          if (joystick.values[ODROID_INPUT_UP])   in |= 1 << 1;   /* GAME+UP:   Y     */
          if (joystick.values[ODROID_INPUT_DOWN]) in |= 1 << 3;   /* GAME+DOWN: Start */
        }
      }
    }
    g_input1_state = in;

    sm_apply_language();   /* the game clears it on init, so hold it */

    /* Pacing, frameskip and the speedup toggle all live here — sm was the only
     * core not calling it, which is why it could not hold 60 Hz, why PAUSE+TIME
     * did nothing, and why the pause menu had no FPS to show. When we are behind,
     * the frame still runs (the game must not slow down) but the PPU is told not
     * to draw the pixels, which is the part that costs. */
    bool draw_frame = common_emu_frame_loop();
    g_ppu_skip_render = !draw_frame;

    screen = lcd_get_active_buffer();
    DrawPpuFrame(screen);

    RtlRunFrame(g_input1_state);
    sm_audio_render();

    if (draw_frame) {
      sm_last_frame = screen;
      common_ingame_overlay();
      sm_sound_submit();
      lcd_swap();
    }

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
