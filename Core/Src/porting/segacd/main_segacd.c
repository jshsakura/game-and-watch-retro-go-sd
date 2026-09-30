#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "odroid_system.h"
#include "odroid_overlay.h"
#include "common.h"
#include "gw_lcd.h"
#include "gw_linker.h"
#include "gw_malloc.h"
#include "rom_manager.h"
#include "rg_storage.h"
#include "gw_flash_alloc.h"
#include "appid.h"
#include "odroid_settings.h"
#include "gw_ofw.h"

#include "pico/pico_types.h"
#include "pico/pico.h"
#include "pico/pico_int.h"
#include "pico/state.h"
#include "porting/segacd/segacd_boot_start.h"
#include "porting/segacd/segacd_config.h"

#define SEGACD_AUDIO_RATE   44100
#define SEGACD_AUDIO_MAX    (SEGACD_AUDIO_RATE / 50 + 16)
#define SEGACD_STATE_MAGIC  0x53434450u /* SCDP */
#define SEGACD_STATE_VER    1u
#define SEGACD_CODE_BASE    0xDF000000u
#define SEGACD_XIP_PATH     "/cores/segacd.xip"
#define SEGACD_STATE_TMP_SIZE 18772u

/* The large memories are intentionally independent objects. PicoCreateMCD
 * requests them through plat_mmap's tagged addresses, allowing the pages to
 * live in four physical SRAM domains. */
static uint8_t segacd_prg_axi[3][0x10000] __attribute__((aligned(4)));
static uint8_t segacd_word_ram[0x40000] __attribute__((aligned(4)));
static uint8_t segacd_sram4_pcm[0x8000]
  __attribute__((section(".segacd_sram4"), aligned(32)));
static uint8_t segacd_ahb_fixed_prg[0x10000]
  __attribute__((section(".segacd_ahb"), aligned(32)));

static uint8_t *segacd_lcd_bonus;
static size_t segacd_lcd_bonus_size;
static void *segacd_state_mem;
static void *segacd_ahb_prg;
static void *segacd_dtcm_prg;
static void *segacd_pcm_mem;
static void *segacd_state_tmp;
static short segacd_snd[SEGACD_AUDIO_MAX];
static uint32_t segacd_clut[256];

/* Device-side state chunks use the LCD pool tail. This avoids asking the
 * remaining DTCM heap for a large transient block after PRG RAM and the CD
 * image descriptor have already been allocated. The weak PicoDrive default
 * still uses malloc in host builds. */
void *gnw_mcd_state_alloc(size_t size)
{
  if (size <= SEGACD_STATE_TMP_SIZE) {
    if (segacd_state_tmp == NULL)
      segacd_state_tmp = ahb_only_malloc(SEGACD_STATE_TMP_SIZE);
    return segacd_state_tmp;
  }
  return NULL;
}

void gnw_mcd_state_free(void *ptr) { (void)ptr; }

void *plat_mmap(unsigned long addr, size_t size, int need_exec, int is_fixed)
{
  (void)need_exec; (void)is_fixed;
  if (addr == 0x05000000 && size == sizeof(mcd_state)) {
    if (segacd_state_mem == NULL) segacd_state_mem = ahb_calloc(1, size);
    return segacd_state_mem;
  }
  if (addr == 0x05100000 && size == 0x10000)
    return segacd_lcd_bonus_size >= 0x10000 ? segacd_lcd_bonus : NULL;
  if (addr >= 0x05200000 && addr < 0x05280000 && size == 0x10000) {
    unsigned page = (unsigned)((addr - 0x05200000) >> 16);
    if (page == 0) return segacd_lcd_bonus_size >= 0x20000 ? segacd_lcd_bonus + 0x10000 : NULL;
    if (page == 1) return segacd_lcd_bonus_size >= 0x30000 ? segacd_lcd_bonus + 0x20000 : NULL;
    if (page == 2) return segacd_ahb_fixed_prg;
    if (page == 3) {
      if (segacd_ahb_prg == NULL) segacd_ahb_prg = ahb_calloc(1, 0x10000);
      return segacd_ahb_prg;
    }
    if (page < 7) return segacd_prg_axi[page - 4];
    if (segacd_dtcm_prg == NULL) segacd_dtcm_prg = calloc(1, 0x10000);
    return segacd_dtcm_prg;
  }
  if (addr == 0x05300000 && size == sizeof(segacd_word_ram))
    return segacd_word_ram;
  if (addr == 0x05400000 && size == 0x8000)
    return segacd_sram4_pcm;
  if (addr == 0x05408000 && size == 0x8000) {
    if (segacd_pcm_mem == NULL) {
      segacd_pcm_mem = ahb_only_malloc(size);
      if (segacd_pcm_mem) memset(segacd_pcm_mem, 0, size);
    }
    return segacd_pcm_mem;
  }
  if (addr == 0x05500000 && size == 0x2000)
    return calloc(1, size);
  return malloc(size);
}

void *plat_mremap(void *ptr, size_t oldsize, size_t newsize)
{ (void)oldsize; return realloc(ptr, newsize); }
void plat_munmap(void *ptr, size_t size) { (void)ptr; (void)size; }
int plat_mem_set_exec(void *ptr, size_t size) { (void)ptr; (void)size; return 0; }

void lprintf(const char *fmt, ...)
{
  va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}

unsigned int crc32_le(unsigned int crc, const unsigned char *buf, unsigned int len);
unsigned long crc32(unsigned long crc, const unsigned char *buf, unsigned int len)
{ return crc32_le((unsigned int)crc, buf, len); }

void *openzip(const char *path) { (void)path; return NULL; }
void closezip(void *zip) { (void)zip; }
int readzip(void *zip) { (void)zip; return -1; }
int seekcompresszip(void *zip, void *ent) { (void)zip; (void)ent; return -1; }
int inflateInit2_(void *s, int w, const char *v, int n)
{ (void)s; (void)w; (void)v; (void)n; return -2; }
int inflate(void *s, int f) { (void)s; (void)f; return -2; }
int inflateReset(void *s) { (void)s; return -2; }
int inflateEnd(void *s) { (void)s; return 0; }
void PicoDrawSetOutputSMS(pdso_t which) { (void)which; }
void PicoDoHighPal555SMS(void) {}
void PicoDraw2SetOutBuf(void *dest, int increment) { (void)dest; (void)increment; }
void PicoDraw2Init(void) { PicoDraw2SetOutBuf(NULL, 0); }
void PicoFrameFull(void) {}
/* PicoFrameStart calls this when the game changes H40/H32 or 224/240 lines,
 * before any line of the new mode is drawn. The margins the new mode no
 * longer covers would keep the old mode's pixels, so wipe the whole frame to
 * the pinned-black index; this frame's lines then draw over it. */
void emu_video_mode_change(int sl, int lc, int sc, int cc)
{
  (void)sl; (void)lc; (void)sc; (void)cc;
  memset(framebuffer1, SEGACD_BORDER_INDEX, 320u * 240u);
}

/* Cartridge-only hardware and compressed CD-audio backends are not part of
 * the first device bring-up. BIN/WAV CDDA and all data tracks remain usable. */
void PicoSVPInit(void) {}
void PicoSVPStartup(void) {}
int mp3_get_bitrate(void *f, int size) { (void)f; (void)size; return -1; }
void mp3_start_play(void *f, int pos) { (void)f; (void)pos; }
void mp3_update(int32_t *buffer, int length, int stereo)
{ (void)buffer; (void)length; (void)stereo; }

static void segacd_set_out(void)
{
  /* PicoDrive can touch the full 240-line output even for a 224-line NTSC
   * mode. Starting at row 8 would overwrite the LCD bonus pool immediately
   * following the 76,800-byte LUT8 framebuffer (where the BIOS lives). */
  PicoDrawSetOutBuf(framebuffer1, 320);
}

static void segacd_push_palette(void);

/* The shared pause dialog always calls repaint. A NULL callback jumps through
 * address zero on device. With one framebuffer there is no clean back buffer
 * to reconstruct; keep the last emulated image and refresh its palette. */
static void segacd_repaint(void)
{
  segacd_set_out();
  segacd_push_palette();
}

static void segacd_push_palette(void)
{
  PicoDrawUpdateHighPal();
  for (unsigned i = 0; i < 256; i++) {
    uint16_t c = Pico.est.HighPal[i];
    uint32_t r = ((c >> 11) & 0x1f) * 255 / 31;
    uint32_t g = ((c >> 5) & 0x3f) * 255 / 63;
    uint32_t b = (c & 0x1f) * 255 / 31;
    segacd_clut[i] = (r << 16) | (g << 8) | b;
  }
  lcd_set_clut_full(segacd_clut, 256);
}

static void segacd_write_sound(int len)
{
  len >>= 1;
  int16_t *dst = audio_get_active_buffer();
  uint16_t cap = audio_get_buffer_length();
  if (common_emu_sound_loop_is_muted()) return;
  int32_t factor = common_emu_sound_get_volume();
  uint16_t n = len < cap ? (uint16_t)len : cap;
  for (uint16_t i = 0; i < n; i++)
    dst[i] = (int16_t)(((int32_t)segacd_snd[i] * factor) >> 8);
  for (uint16_t i = n; i < cap; i++) dst[i] = 0;
}

/* The Sega CD pad is the Genesis 6-button pad and shares APPID_MD's keymap
 * (odroid_settings.c keymap_profiles), so the in-game Controls dialog remaps
 * it. PicoIn.pad bit layout is "MXYZ SACB RLDU". Read from the RAW pad, before
 * the Mario TIME/PAUSE swap, exactly like main_gwenesis.c: while the menu
 * shortcut key is held nothing reaches the console. */
static uint16_t segacd_pad(const odroid_gamepad_state_t *j)
{
  if (j->values[get_ofw_is_mario() ? ODROID_INPUT_SELECT : ODROID_INPUT_VOLUME])
    return 0;
  uint16_t p = 0;
  if (j->values[ODROID_INPUT_UP])    p |= 1u << 0;
  if (j->values[ODROID_INPUT_DOWN])  p |= 1u << 1;
  if (j->values[ODROID_INPUT_LEFT])  p |= 1u << 2;
  if (j->values[ODROID_INPUT_RIGHT]) p |= 1u << 3;
  if (odroid_keymap_pressed(j, ODROID_KEYMAP_MD_B))     p |= 1u << 4;
  if (odroid_keymap_pressed(j, ODROID_KEYMAP_MD_C))     p |= 1u << 5;
  if (odroid_keymap_pressed(j, ODROID_KEYMAP_MD_A))     p |= 1u << 6;
  if (odroid_keymap_pressed(j, ODROID_KEYMAP_MD_START)) p |= 1u << 7;
  if (odroid_keymap_pressed(j, ODROID_KEYMAP_MD_Z))     p |= 1u << 8;
  if (odroid_keymap_pressed(j, ODROID_KEYMAP_MD_Y))     p |= 1u << 9;
  if (odroid_keymap_pressed(j, ODROID_KEYMAP_MD_X))     p |= 1u << 10;
  if (odroid_keymap_pressed(j, ODROID_KEYMAP_MD_MODE))  p |= 1u << 11;
  return p;
}

static void segacd_swap_menu_keys(odroid_gamepad_state_t *j)
{
  uint8_t key = j->values[ODROID_INPUT_VOLUME];
  j->values[ODROID_INPUT_VOLUME] = j->values[ODROID_INPUT_SELECT];
  j->values[ODROID_INPUT_SELECT] = key;
}

/* ---- backup RAM -----------------------------------------------------------
 * The console's internal 8K BRAM is where Sega CD games keep their saves, and
 * PicoDrive formats it blank at every power-on. It lives in <rom>.brm (the
 * SRAM path every core uses), is loaded once the disc is in, and is written
 * back whenever its contents change -- checked once a second, so a save made
 * in-game survives pulling the battery, not only a clean exit -- and from the
 * shutdown/sleep hook. */
#define SEGACD_BRAM_SIZE        0x2000u
#define SEGACD_BRAM_CHECK_EVERY 60u
static uint32_t segacd_bram_crc;

static uint32_t segacd_bram_hash(void)
{
  return crc32_le(0, Pico_mcd->bram, SEGACD_BRAM_SIZE);
}

extern unsigned char formatted_bram[4 * 0x10];

/* What PicoPowerMCD leaves in BRAM: blank, with the format footer at the end. */
static void segacd_bram_format(void)
{
  memset(Pico_mcd->bram, 0, SEGACD_BRAM_SIZE);
  memcpy(Pico_mcd->bram + SEGACD_BRAM_SIZE - sizeof(formatted_bram),
         formatted_bram, sizeof(formatted_bram));
}

static void segacd_bram_load(void)
{
  char *path = odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ACTIVE_FILE->path);
  if (path == NULL) return;
  FILE *f = fopen(path, "rb");
  if (!f) segacd_bram_format();
  else {
    /* Only a whole image replaces the formatted default: a short file would
     * leave a half-old, half-blank BRAM that the BIOS treats as corrupt. No
     * staging buffer -- the DTCM heap already holds a PRG page. */
    if (fseek(f, 0, SEEK_END) == 0 && ftell(f) == (long)SEGACD_BRAM_SIZE &&
        fseek(f, 0, SEEK_SET) == 0)
      (void)fread(Pico_mcd->bram, 1, SEGACD_BRAM_SIZE, f);
    fclose(f);
  }
  free(path);
  segacd_bram_crc = segacd_bram_hash();
}

static void segacd_bram_save(void)
{
  uint32_t crc = segacd_bram_hash();
  if (crc == segacd_bram_crc) return;
  char *path = odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ACTIVE_FILE->path);
  if (path == NULL) return;
  FILE *f = fopen(path, "wb");
  if (f) {
    bool ok = fwrite(Pico_mcd->bram, 1, SEGACD_BRAM_SIZE, f) == SEGACD_BRAM_SIZE;
    if (fclose(f) != 0) ok = false;
    if (ok) segacd_bram_crc = crc;   /* a failed write is retried next check */
  }
  free(path);
}

static size_t state_read(void *p, size_t s, size_t n, void *f) { return fread(p,s,n,(FILE *)f); }
static size_t state_write(void *p, size_t s, size_t n, void *f) { return fwrite(p,s,n,(FILE *)f); }
static size_t state_eof(void *f) { return (size_t)feof((FILE *)f); }
static int state_seek(void *f, long o, int w) { return fseek((FILE *)f,o,w); }

static bool segacd_save(const char *path)
{
  uint32_t h[2] = { SEGACD_STATE_MAGIC, SEGACD_STATE_VER };
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(h, sizeof(h), 1, f) == 1 &&
    PicoStateFP(f, 1, state_read, state_write, state_eof, state_seek) == 0;
  if (fclose(f) != 0) ok = false;
  if (!ok) remove(path);
  return ok;
}

static bool segacd_load(const char *path)
{
  uint32_t h[2];
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  /* A savestate carries BRAM, but BRAM is the game's own save file and must
   * not be rolled back by loading an older snapshot: flush it first, then put
   * the file's contents back over whatever the state held. */
  segacd_bram_save();
  bool ok = fread(h, sizeof(h), 1, f) == 1 && h[0] == SEGACD_STATE_MAGIC &&
    h[1] == SEGACD_STATE_VER &&
    PicoStateFP(f, 0, state_read, state_write, state_eof, state_seek) == 0;
  fclose(f);
  segacd_bram_load();
  if (ok) {
    memset(framebuffer1, SEGACD_BORDER_INDEX, 320u * 240u);
    segacd_set_out();
    segacd_push_palette();
  }
  return ok;
}

static void *segacd_screenshot(void) { lcd_wait_for_vblank(); return framebuffer1; }
static int segacd_fps = 60;
static void segacd_sleep_wake(void)
{
  common_emu_auto_oc(2);
  audio_start_playing(SEGACD_AUDIO_RATE / segacd_fps);
}

static int patch_sentinels(uint32_t *p, uint32_t *end, int32_t off, uint32_t size)
{
  int n = 0;
  while (p < end) {
    uint32_t v = *p;
    if ((v & ~1u) >= SEGACD_CODE_BASE && (v & ~1u) < SEGACD_CODE_BASE + size) {
      *p = v + off; n++;
    }
    p++;
  }
  return n;
}

static void relocate_xip(uint8_t *buf, uint32_t len, uint32_t file_off,
                         uint8_t *file_addr, uint32_t file_size)
{
  (void)file_off;
  int32_t off = (int32_t)((uintptr_t)file_addr - SEGACD_CODE_BASE);
  patch_sentinels((uint32_t *)buf, (uint32_t *)(buf + (len & ~3u)), off, file_size);
}

extern uint8_t __segacd_itc_start__[], __segacd_itc_end__[];

static bool cache_xip(void)
{
  uint32_t size = 0;
  uint8_t *addr = odroid_overlay_cache_file_in_flash_relocate(
    SEGACD_XIP_PATH, &size, false, relocate_xip);
  if (!addr || !size) return false;
  int32_t off = (int32_t)((uintptr_t)addr - SEGACD_CODE_BASE);
  patch_sentinels((uint32_t *)&__RAM_EMU_START__,
                  (uint32_t *)&_OVERLAY_SEGACD_BSS_START, off, size);
  /* ITCM code calls into segacd.xip through veneers with sentinel literals. */
  patch_sentinels((uint32_t *)__segacd_itc_start__,
                  (uint32_t *)__segacd_itc_end__, off, size);
  __DSB(); __ISB();
  return true;
}

static const char *bios_path_for_region(int region)
{
  if (region == 8) return "/bios/segacd/bios_CD_E.bin";
  if (region == 1 || region == 2) return "/bios/segacd/bios_CD_J.bin";
  return "/bios/segacd/bios_CD_U.bin";
}

void app_main_segacd(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
  odroid_gamepad_state_t joystick;
  segacd_boot_start_t boot_start;
  odroid_dialog_choice_t options[] = { ODROID_DIALOG_CHOICE_LAST };
  common_emu_state.pause_after_frames = start_paused ? 2 : 0;
  if (start_paused) odroid_audio_mute(true);
  common_emu_state.frame_time_10us = 1667;
  common_emu_auto_oc(2);
  /* The 64K SRAM4 bank is clock-gated after reset.  One PRG RAM page lives
   * there; the first memset otherwise raises an imprecise BusFault. */
  __HAL_RCC_SRDSRAM_CLK_ENABLE();
  /* A 64K PRG page occupies AHB 0x30000000..0x3000ffff. Other cores' static
   * AHB overlays end at __ahbram_heap_start__; skip their overlapping tail
   * before allocating Sega CD's PCM upper half and state scratch. */
  extern uint8_t __ahbram_heap_start__[];
  if ((uintptr_t)__ahbram_heap_start__ < 0x30010000u)
    (void)ahb_only_malloc(0x30010000u - (uintptr_t)__ahbram_heap_start__);

  extern void *_OVERLAY_SEGACD_BSS_END[];
  ram_start = (uint32_t)&_OVERLAY_SEGACD_BSS_END;
  odroid_system_init(APPID_SEGACD, SEGACD_AUDIO_RATE);
  /* odroid_system_init() initializes LTDC in the default RGB565 layout, so
   * switch formats only afterwards; doing this before init is overwritten. */
  lcd_setup_single_framebuffer(LCD_MODE_LUT8);
  lcd_set_refresh_rate(60);
  lcd_get_bonus_pool(&segacd_lcd_bonus, &segacd_lcd_bonus_size);
  if (segacd_lcd_bonus_size < 0x30000) {
    odroid_overlay_alert("Sega CD LCD memory unavailable");
    odroid_system_switch_app(0); return;
  }
  extern uint8_t __segacd_lcd_aux_start__[], __segacd_lcd_aux_end__[];
  memset(__segacd_lcd_aux_start__, 0,
         (size_t)(__segacd_lcd_aux_end__ - __segacd_lcd_aux_start__));
  odroid_system_emu_init(segacd_load, segacd_save, segacd_screenshot,
                         NULL, segacd_sleep_wake, segacd_bram_save, NULL);

  if (!cache_xip()) {
    odroid_overlay_alert("Missing /cores/segacd.xip");
    odroid_system_switch_app(0); return;
  }

  PicoInit();
  PicoIn.opt = SEGACD_PICO_OPT;
  PicoIn.sndRate = SEGACD_AUDIO_RATE;
  PicoIn.autoRgnOrder = 0x184;

  int region = 4;
  (void)PicoCdCheck(ACTIVE_FILE->path, &region);
  const char *bios_path = bios_path_for_region(region);
  uint32_t bios_size = 0;
  gnw_mcd_bios_xip = odroid_overlay_cache_file_in_flash(bios_path, &bios_size, true);
  gnw_mcd_bios_xip_size = bios_size;
  if (!gnw_mcd_bios_xip || gnw_mcd_bios_xip_size < 0x20000) {
    odroid_overlay_alert("Missing Sega CD BIOS in /bios/segacd");
    odroid_system_switch_app(0); return;
  }

  enum media_type_e mt = PicoLoadMedia(ACTIVE_FILE->path, NULL, 0,
                                        NULL, NULL, NULL, NULL);
  if (mt == PM_ERROR || mt == PM_BAD_CD || mt == PM_BAD_CD_NO_BIOS) {
    odroid_overlay_alert("Unsupported Sega CD image");
    odroid_system_switch_app(0); return;
  }

  /* Loading the media powered the MCD, which formats BRAM; the saved one
   * goes over it now, before the BIOS or the game reads it. */
  segacd_bram_load();

  segacd_fps = Pico.m.pal ? 50 : 60;
  int fps = segacd_fps;
  common_emu_state.frame_time_10us = (uint16_t)(100000 / fps);
  PicoLoopPrepare();
  PicoSetInputDevice(0, PICO_INPUT_PAD_6BTN);
  PicoIn.sndOut = segacd_snd;
  PicoIn.writeSound = segacd_write_sound;
  PsndRerate(0);
  PicoDrawSetOutFormat(PDF_8BIT, 0);
  segacd_set_out();
  segacd_push_palette();
  audio_start_playing(SEGACD_AUDIO_RATE / fps);

  /* A loaded state is already past the BIOS menu. */
  segacd_boot_start_init(&boot_start, !load_state);
  memset(framebuffer1, SEGACD_BORDER_INDEX, 320u * 240u);
  if (load_state) odroid_system_emu_load_state(save_slot);

  uint32_t bram_tick = 0;
  while (1) {
    wdog_refresh();
    bool draw = common_emu_frame_loop();
    odroid_input_read_gamepad(&joystick);
    /* Same as the MD port: on Mario units raw TIME is the menu key and raw
     * PAUSE/SET is left to the keymap. Swap for the shared menu handler, then
     * swap back so the keymap (and turbo) see the raw keys again. */
    bool mario = get_ofw_is_mario();
    if (mario) segacd_swap_menu_keys(&joystick);
    common_emu_input_loop(&joystick, options, &segacd_repaint);
    common_emu_input_loop_handle_turbo(&joystick);
    if (mario) segacd_swap_menu_keys(&joystick);
    PicoIn.pad[0] = segacd_pad(&joystick) |
                    segacd_boot_start_pad(&boot_start, SekPc);
    if (++bram_tick >= SEGACD_BRAM_CHECK_EVERY) {
      bram_tick = 0;
      segacd_bram_save();
    }
    PicoIn.skipFrame = draw ? 0 : 1;
    if (draw) segacd_set_out();
    PicoFrame();
    if (draw) {
      segacd_push_palette();
      common_ingame_overlay();
      lcd_swap();
    }
    common_emu_sound_sync(false);
  }
}
