/* Sega 32X (picodrive) as a launcher system — EXPERIMENTAL baseline.
 *
 * The core substrate is picodrive (external/picodrive): Genesis 68K (FAME) +
 * Z80 (cz80) + VDP + dual SH-2 interpreter + the 32X VDP/PWM. Genesis (.md) is
 * served by the separate lean gwenesis core; this core exists ONLY because the
 * SH-2s and 32X hardware are not in gwenesis. Same picodrive binary can later
 * enable pico/cd for Sega CD (PAHW_MCD), but that is a separate RAM problem.
 *
 * Memory discipline (the 32X is RAM-tight — see docs / sega32x memory notes):
 *   - ROM stays memory-mapped in external flash (flash-cache), zero-copy: the
 *     GNW_32X_CORE guard in picodrive/pico/cart.c points Pico.rom straight at
 *     the flash image instead of plat_mmap-ing a 2 MB RAM copy.
 *   - Pico32xMem (SDRAM 256K + DRAM framebuffers 256K) is a static buffer under
 *     GNW_32X_CORE (picodrive/pico/32x/32x.c) instead of calloc, so the linker
 *     sees it and the RAM_EMU ASSERT is honest.
 *   - The fat runtime tables are treated at build time: cz80 SZHVC computed
 *     (CZ80_BIG_FLAGS_ARRAY=0), YM2612 tables const/XIP, draw2 dropped.
 *
 * The full-init sequence below mirrors the libretro frontend exactly; a partial
 * init is what hung the QEMU rig on the first PicoFrame (68K spun before the
 * SH-2 clock multiplier was set). Do not drop a step.
 */
#include <odroid_system.h>

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "gw_lcd.h"
#include "gw_linker.h"
#include "gw_buttons.h"
#include "rom_manager.h"
#include "common.h"
#include "gw_malloc.h"
#include "rg_storage.h"
#include "odroid_overlay.h"
#include "gw_flash_alloc.h"   /* odroid_overlay_cache_file_in_flash_relocate */
#include "appid.h"
#include "main_md32x.h"

#include "pico/pico_types.h"   /* s8/s16/s32 — MUST precede pico.h */
#include "pico/pico.h"
#include "pico/pico_int.h"     /* Pico.est.Draw2FB binding (Draw2 shim below) */

/* ---- geometry / rates ----------------------------------------------------- */
#define MD32X_FPS            60
#define MD32X_WIDTH          320          /* 32X is H40 (320) most of the time  */
#define MD32X_HEIGHT         224
#define MD32X_TOP_MARGIN     ((240 - MD32X_HEIGHT) / 2)   /* 8 lines            */
#define MD32X_AUDIO_RATE     44100
#define MD32X_AUDIO_MAX      (MD32X_AUDIO_RATE / 50 + 16) /* worst-case/frame   */

/* Savestate stamp: refuse a file this build did not write (project rule). The
 * payload itself is picodrive's own versioned PicoState stream. */
#define MD32X_STATE_MAGIC    0x4D583258u  /* "MX2X" */
#define MD32X_STATE_VERSION  1

/* ---- picodrive platform hooks --------------------------------------------
 * On the device there is no large malloc heap. With the GNW_32X_CORE guards
 * (ROM zero-copy + static Pico32xMem) picodrive should not plat_mmap anything
 * big; keep tiny stubs so a stray call is caught rather than silently wild. */
void *plat_mmap(unsigned long addr, size_t size, int need_exec, int is_fixed) {
  (void)addr; (void)need_exec; (void)is_fixed;
  /* Only reached if a guard is missing — flag it loudly at bring-up. */
  return malloc(size);
}
void *plat_mremap(void *ptr, size_t oldsize, size_t newsize) {
  (void)oldsize; return realloc(ptr, newsize);
}
void plat_munmap(void *ptr, size_t size) { (void)size; free(ptr); }
int  plat_mem_set_exec(void *ptr, size_t size) { (void)ptr; (void)size; return 0; }

/* ---- shims for externs the trimmed picodrive build leaves unbound ---------
 * None of these names appear in md32x_redefines (only DEFINED overlay symbols
 * are harvested), so picodrive's references bind here unrenamed. */
void lprintf(const char *fmt, ...) {           /* picodrive logging hook */
  va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
/* cart.c computes ROM CRCs (carthw detection): forward to the firmware's
 * zlib-compatible table (porting/crc32.c) — values must be REAL, not stubbed. */
unsigned int crc32_le(unsigned int crc, unsigned char const *buf, unsigned int len);
unsigned long crc32(unsigned long crc, const unsigned char *buf, unsigned int len) {
  return crc32_le((unsigned int)crc, buf, len);
}
/* zip/gzip/cso media loading is a desktop-frontend path — the device hands
 * PicoLoadMedia a raw flash-mapped .32x image. Honest fail-stubs (NULL / zlib
 * Z_STREAM_ERROR), never silent success. */
void *openzip(const char *path) { (void)path; return NULL; }
void  closezip(void *zip) { (void)zip; }
int   readzip(void *zip) { (void)zip; return -1; }
int   seekcompresszip(void *zip, void *ent) { (void)zip; (void)ent; return -1; }
int   inflateInit2_(void *strm, int wbits, const char *ver, int ssize)
      { (void)strm; (void)wbits; (void)ver; (void)ssize; return -2; }
int   inflate(void *strm, int flush) { (void)strm; (void)flush; return -2; }
int   inflateReset(void *strm) { (void)strm; return -2; }
int   inflateEnd(void *strm) { (void)strm; return 0; }
/* SMS renderer TU is excluded; unreachable for 32X. */
void PicoDrawSetOutputSMS(pdso_t which) { (void)which; }

/* Draw2FB: with draw2.c excluded this binding still matters — the 32X
 * compositor points the MD line renderer INTO this 328x(8+240+8) CLUT frame
 * and reads it back per-pixel (pmd) for MD/32X layer priority. A no-op stub
 * left it NULL = wild reads (QEMU rig proof: SIGSEGV at pmd=0xa48; on the
 * device a Hardfault). ~84K, from AHB at load — per-core, nobody's pool
 * shrinks (the m68k bank no longer lives there, see below). */
#define MD32X_D2FB_LINE   328
#define MD32X_D2FB_BYTES  (MD32X_D2FB_LINE * (8 + 240 + 8) + 8)
static uint8_t *md32x_draw2fb;

void PicoDraw2SetOutBuf(void *dest, int increment) {
  /* draw2.c's binding, verbatim minus the renderer (cf. rig_32x_draw2fb.c).
   * References to Pico resolve to md32x__Pico — the redefine pass renames
   * references in this object too, so this binds the overlay's own state. */
  if (dest) {
    Pico.est.Draw2FB = dest;
    Pico.est.Draw2Width = increment;
  } else {
    if (md32x_draw2fb == NULL)
      md32x_draw2fb = (uint8_t *)ahb_calloc(1, MD32X_D2FB_BYTES);
    Pico.est.Draw2FB = md32x_draw2fb;
    Pico.est.Draw2Width = MD32X_D2FB_LINE;
  }
}

/* The 68K page-0 image is NOT a 64K RAM copy anymore: reads >= 0x100 come
 * straight from the flash-mapped ROM via PicoRead8/16_gnwbank (fork), and
 * only the synthesized 0x100 stub (incl. the writable H-int vector at 0x70)
 * needs RAM. 256 bytes, static. */
static unsigned char md32x_m68k_stub[0x100];
unsigned char *gnw_m68k_bank_alloc(void) {
  return md32x_m68k_stub;
}

/* ---- audio ----------------------------------------------------------------
 * Mono (POPT_EN_STEREO off) keeps RAM and the device audio path simple. The
 * core fills md32x_snd during PicoFrame and calls md32x_write_sound(len) once
 * per frame; we scale by the launcher volume and hand it to the mixer. */
static short md32x_snd[MD32X_AUDIO_MAX];

static void md32x_write_sound(int len) {
  len >>= 1;   /* picodrive passes BYTES (sound.c: curr_pos * 2 for mono) —
                * NTSC only worked by accident through the min() clamp (audit) */
  int16_t *dst = audio_get_active_buffer();
  uint16_t dst_len = audio_get_buffer_length();
  if (common_emu_sound_loop_is_muted()) return;
  int32_t factor = common_emu_sound_get_volume();
  uint16_t n = (uint16_t)((len < dst_len) ? len : dst_len);
  for (uint16_t i = 0; i < n; i++)
    dst[i] = (int16_t)(((int32_t)md32x_snd[i] * factor) >> 8);
  for (uint16_t i = n; i < dst_len; i++)
    dst[i] = 0;
}

/* ---- input ----------------------------------------------------------------
 * picodrive PicoIn.pad[] bit layout is "MXYZ SACB RLDU":
 *   0=U 1=D 2=L 3=R 4=B 5=C 6=A 7=Start 8=Z 9=Y 10=X 11=Mode  (1 = pressed).
 * Two face buttons on the unit: A->Genesis A, B->Genesis B; GAME->C so the
 * common 3-button games are fully playable; START as itself, SELECT->Mode for
 * the handful of 6-button/menu games. (Tunable later like gwenesis's ABC menu.)
 */
static uint16_t read_md_pad(odroid_gamepad_state_t *j) {
  uint16_t p = 0;
  if (j->values[ODROID_INPUT_UP])     p |= 1u << 0;
  if (j->values[ODROID_INPUT_DOWN])   p |= 1u << 1;
  if (j->values[ODROID_INPUT_LEFT])   p |= 1u << 2;
  if (j->values[ODROID_INPUT_RIGHT])  p |= 1u << 3;
  if (j->values[ODROID_INPUT_B])      p |= 1u << 4;   /* B -> Genesis B */
  if (j->values[ODROID_INPUT_X])      p |= 1u << 5;   /* GAME -> Genesis C */
  if (j->values[ODROID_INPUT_A])      p |= 1u << 6;   /* A -> Genesis A */
  if (j->values[ODROID_INPUT_START])  p |= 1u << 7;
  if (j->values[ODROID_INPUT_SELECT]) p |= 1u << 11;  /* Mode */
  return p;
}

/* ---- video ----------------------------------------------------------------
 * Direct-render into the LCD active buffer (gwenesis-proven, RAM-lean vs a
 * dedicated framebuffer). Robust against double-buffer swap-staleness because
 * both buffers start cleared, picodrive paints the full raster each drawn
 * frame, and we swap ONLY on drawn frames (skipped frames leave the shown
 * buffer intact). Geometry is centered from picodrive's own mode callback so
 * H32(256)/H40(320) and PAL/NTSC line counts stay centered with no stale
 * margins. */
static int md32x_out_line = (240 - 224) / 2;   /* top margin, updated on mode change */
static int md32x_out_col  = 0;                 /* left margin */

static void set_out_buffer(void) {
  uint16_t *fb = lcd_get_active_buffer();
  PicoDrawSetOutBuf(fb + md32x_out_line * 320 + md32x_out_col, 320 * 2);
}

/* picodrive frontend hooks (referenced by pico/draw.c and pico/32x/32x.c). Not
 * in md32x_redefines, so the core's extern refs resolve straight here.
 *
 * Fired by the LAZY Pico32xStartup (the game's own 68K writes ADEN at
 * 0xA15101). PicoDrawSetOutFormat/SetOutBuf route to their 32X variants only
 * once PAHW_32X is set, so they MUST be re-applied here — otherwise the 32X
 * layer renders into picodrive's internal DefOutBuff and the screen stays
 * black (QEMU-rig-proven; libretro's hook does the same re-apply). */
void emu_32x_startup(void) {
  PicoDrawSetOutFormat(PDF_RGB555, 0);
  set_out_buffer();
}

void emu_video_mode_change(int start_line, int line_count, int start_col, int col_count) {
  (void)start_line; (void)start_col;
  md32x_out_line = (240 - line_count) / 2; if (md32x_out_line < 0) md32x_out_line = 0;
  md32x_out_col  = (320 - col_count)  / 2; if (md32x_out_col  < 0) md32x_out_col  = 0;
  set_out_buffer();
}

/* ---- savestate (M2) --------------------------------------------------------
 * DEFERRED for the M1 feasibility milestone (boot + fps first). picodrive's
 * PicoState(fname,is_save) in pico/state.c is entangled with ym2413 (SMS FM),
 * megasd (Sega CD) and zlib — none of which the trimmed 32X build compiles.
 * M2 wires it behind these stubs by guarding those chunks out in state.c (a
 * GNW_32X_CORE fork guard) and stamping the file with MD32X_STATE_MAGIC.
 * Returning false here is honest: the launcher shows save/load as unavailable
 * rather than silently doing nothing (the "never wired" trap). */
static bool md32x_SaveState(const char *pathName) { (void)pathName; return false; }
static bool md32x_LoadState(const char *pathName) { (void)pathName; return false; }

static void *md32x_Screenshot(void) {
  lcd_wait_for_vblank();
  return lcd_get_active_buffer();
}

static void diag_log(const char *fmt, ...);   /* boot diag, defined below */

/* ---- XIP: cold code + rodata from flash (the SM/GBA sentinel pattern) ------
 * .xip_md32x + .rodata_md32x are linked at MD32X_CODE_BASE (a sentinel — nothing
 * lives there), shipped as /cores/32x.xip, cached into QSPI at load, and every
 * pointer into the sentinel range is rewritten by +offset. The scan skips this
 * file's own window [_MD32X_MAIN_CODE_START, _MD32X_MAIN_CODE_END): the literal
 * constant below must not be "relocated". */
#define MD32X_CODE_BASE  0xDEB00000u
#define MD32X_XIP_PATH   "/cores/32x.xip"

extern uint32_t _MD32X_MAIN_CODE_START[], _MD32X_MAIN_CODE_END[];
extern uint8_t __md32x_itc_bss_start__[], __md32x_itc_bss_end__[];

static uint8_t *g_xip_addr;
static uint32_t g_xip_size;
static int32_t  g_xip_offset;

static int PatchMd32xSentinels(uint32_t *start, uint32_t *end, int32_t offset, uint32_t size) {
  int patched = 0;
  for (uint32_t *p = start; p < end; p++) {
    uint32_t v = *p;
    if ((v & ~1u) >= MD32X_CODE_BASE && (v & ~1u) < MD32X_CODE_BASE + size) {
      *p = (uint32_t)(v + offset);   /* Thumb bit rides along */
      patched++;
    }
  }
  return patched;
}

static void Md32xRelocateXip(uint8_t *buffer, uint32_t length, uint32_t offset_in_file,
                             uint8_t *file_address, uint32_t file_size) {
  (void)offset_in_file;
  int32_t offset = (int32_t)((uint32_t)file_address - MD32X_CODE_BASE);
  PatchMd32xSentinels((uint32_t *)buffer, (uint32_t *)(buffer + (length & ~3u)), offset, file_size);
}

static bool Md32xCacheXipToFlash(void) {
  extern uint8_t __xip_md32x_start__[], __xip_md32x_end__[];
  const uint32_t expected = (uint32_t)(__xip_md32x_end__ - __xip_md32x_start__);
  g_xip_size = 0;
  g_xip_addr = odroid_overlay_cache_file_in_flash_relocate(MD32X_XIP_PATH, &g_xip_size,
                                                           false, &Md32xRelocateXip);
  if (g_xip_addr == NULL || g_xip_size == 0)
    return false;
  /* A stale 32x.xip against a new firmware = mismatched sentinel offsets =
   * wild jumps with no message (audit: bin is CORI-tagged, xip was not).
   * Size is a strong cheap proxy — refuse loudly instead of booting garbage. */
  if (g_xip_size != expected) {
    diag_log("FATAL: 32x.xip size %lu != firmware's %lu (stale xip? re-copy /cores)\n",
             (unsigned long)g_xip_size, (unsigned long)expected);
    return false;
  }
  g_xip_offset = (int32_t)((uint32_t)g_xip_addr - MD32X_CODE_BASE);
  return true;
}

/* ---- one-shot boot diagnostic (strip later) --------------------------------
 * The SNES /snes_diag.txt pattern: breadcrumbs accumulate in RAM and the file
 * is REWRITTEN (open/write/close) at each boot stage, so a crash mid-boot
 * leaves the last completed stage on the SD — read the file on a PC instead
 * of photographing a BSOD. HARD RULE: never written after the main loop
 * starts (mid-play SD writes corrupt the card — the /snes_diag saga). The
 * headless warm-up below runs BEFORE audio starts, so its writes are
 * boot-time too. */
#define MD32X_DIAG_PATH "/32x_diag.txt"
static char md32x_diag[2048];
static uint16_t md32x_diag_len;
static bool md32x_diag_sealed;   /* true once the main loop starts: no more writes */

static void diag_log(const char *fmt, ...) {
  if (md32x_diag_sealed) return;
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(md32x_diag + md32x_diag_len,
                    sizeof(md32x_diag) - md32x_diag_len, fmt, ap);
  va_end(ap);
  if (n > 0) {
    md32x_diag_len += (uint16_t)((n < (int)(sizeof(md32x_diag) - md32x_diag_len))
                                 ? n : (int)(sizeof(md32x_diag) - md32x_diag_len) - 1);
  }
  wdog_refresh();
  FILE *f = fopen(MD32X_DIAG_PATH, "wb");
  if (f) { fwrite(md32x_diag, 1, md32x_diag_len, f); fclose(f); }
}

/* ---- ROM load ------------------------------------------------------------- */
static const uint8_t *md32x_rom;
static uint32_t md32x_rom_len;

void app_main_md32x(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
  odroid_gamepad_state_t joystick;
  odroid_dialog_choice_t options[] = { ODROID_DIALOG_CHOICE_LAST };

  if (start_paused) {
    common_emu_state.pause_after_frames = 2;
    odroid_audio_mute(true);
  } else {
    common_emu_state.pause_after_frames = 0;
  }
  common_emu_state.frame_time_10us = (uint16_t)(100000 / MD32X_FPS + 0.5f);
  lcd_set_refresh_rate(MD32X_FPS);

  /* Dual-SH-2 interpreter is CPU-bound; take the scoped, non-persisted boost. */
  common_emu_auto_oc(1);

  /* Spare-RAM allocator base (gwenesis pattern). ahb_calloc() tries ram_malloc
   * FIRST and ram_malloc asserts on ram_start==0 — first device boot died
   * exactly there when gnw_m68k_bank_alloc() ran. The overlay margin (~9K) is
   * too small for the 64K bank, so it falls through to AHB as intended. */
  extern void *_OVERLAY_MD32X_BSS_END[];
  ram_start = (uint32_t)&_OVERLAY_MD32X_BSS_END;

  odroid_system_init(APPID_32X, MD32X_AUDIO_RATE);
  odroid_system_emu_init(&md32x_LoadState, &md32x_SaveState, &md32x_Screenshot,
                         NULL, NULL, NULL);
  /* audio_start_playing happens AFTER the warm-up frame below, with the
   * region-correct per-frame sample count (audit bug: PAL carts pace 50fps). */

  /* ROM: memory-mapped in external flash, read-only, zero-copy (cart.c's
   * GNW_32X_CORE guard binds Pico.rom here rather than copying). The zero-copy
   * path SKIPS picodrive's in-place Byteswap(), and its 68K/SH-2 handlers
   * expect the ROM stored 16-bit-word-swapped — so cache it byteswapped here
   * (true), the swap the normal PicoCartLoad would have done. */
  diag_log("32x diag v1 (boot breadcrumbs; last line = last completed stage)\n"
           "rom=%s\n", ACTIVE_FILE->path);

  uint32_t sz = 0;
  const uint8_t *rom = odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &sz, true);
  md32x_rom = rom; md32x_rom_len = sz;
  diag_log("rom cached: addr=%p len=%lu head=%02x%02x%02x%02x\n",
           (void *)rom, (unsigned long)sz,
           rom ? rom[0] : 0, rom ? rom[1] : 0, rom ? rom[2] : 0, rom ? rom[3] : 0);

  /* XIP cold code+rodata MUST be cached and the overlay's sentinel pointers
   * patched BEFORE the first picodrive call — PicoInit itself lives in XIP. */
  if (!Md32xCacheXipToFlash()) {
    diag_log("FATAL: xip cache failed (missing %s?)\n", MD32X_XIP_PATH);
    odroid_overlay_alert("Missing " MD32X_XIP_PATH " - re-run the update");
    odroid_system_switch_app(0);
    return;
  }
  diag_log("xip cached: addr=%p size=%lu off=%ld\n",
           (void *)g_xip_addr, (unsigned long)g_xip_size, (long)g_xip_offset);
  {
    int n1 = PatchMd32xSentinels((uint32_t *)&__RAM_EMU_START__, _MD32X_MAIN_CODE_START,
                                 g_xip_offset, g_xip_size);
    int n2 = PatchMd32xSentinels(_MD32X_MAIN_CODE_END, (uint32_t *)&_OVERLAY_MD32X_BSS_START,
                                 g_xip_offset, g_xip_size);
    diag_log("sentinels patched: %d+%d\n", n1, n2);
  }

  /* Hot writable state (bus map tables, YM/Z80/draw contexts) lives in ITCM
   * (.overlay_md32x_itc_bss) — outside the overlay BSS the launcher memsets,
   * so zero it here or it starts as the previous core's ITCM contents. */
  memset(__md32x_itc_bss_start__, 0,
         (size_t)(__md32x_itc_bss_end__ - __md32x_itc_bss_start__));
  diag_log("itc zeroed: %u B\n",
           (unsigned)(__md32x_itc_bss_end__ - __md32x_itc_bss_start__));

  /* --- picodrive init, libretro (upstream frontend) order — QEMU-rig-proven.
   * 32X startup is LAZY: the game's own MD-mode boot code writes ADEN at
   * 0xA15101 and PicoWrite8_32x calls Pico32xStartup (which runs
   * Pico32xPrepare + our emu_32x_startup hook). Calling Pico32xStartup up
   * front — the old order — pre-enables the adapter and breaks the boot
   * handshake (VF's 68K parks in an idle loop forever). PicoReset is not
   * called either: PicoLoadMedia -> PicoCartInsert -> PicoPower already
   * reset the machine. */
  PicoInit();
  PicoIn.opt = POPT_EN_FM | POPT_EN_PSG | POPT_EN_Z80
             | POPT_EN_32X | POPT_EN_PWM
             | POPT_ACC_SPRITES | POPT_DIS_32C_BORDER;   /* mono: no EN_STEREO */
  PicoIn.sndRate = MD32X_AUDIO_RATE;
  PicoIn.autoRgnOrder = 0x184;   /* US, EU, JP */

  enum media_type_e mt = PicoLoadMedia(ACTIVE_FILE->path, (unsigned char *)rom, sz,
                                       NULL, NULL, NULL, NULL);
  if (mt == PM_ERROR || rom == NULL) {
    odroid_overlay_alert("Unsupported / bad 32X ROM");
    odroid_system_switch_app(0);
    return;
  }

  diag_log("PicoLoadMedia: mt=%d AHW=%x romsize=%lu pal=%d\n",
           (int)mt, (unsigned)PicoIn.AHW, (unsigned long)Pico.romsize,
           (int)Pico.m.pal);

  /* Region pacing (audit: a PAL-only cart selected via autoRgnOrder ran 20%
   * fast with 147 samples/frame silently dropped). gwenesis does the same. */
  int md32x_fps = Pico.m.pal ? 50 : 60;
  common_emu_state.frame_time_10us = (uint16_t)(100000 / md32x_fps);
  lcd_set_refresh_rate(md32x_fps);

  PicoLoopPrepare();
  PicoIn.sndOut = md32x_snd;
  PicoIn.writeSound = md32x_write_sound;
  PsndRerate(0);
  PicoDrawSetOutFormat(PDF_RGB555, 0);   /* despite the name: RGB565 out on this config */
  set_out_buffer();
  diag_log("init done (draw fmt set, snd ready)\n");

  /* One headless warm-up frame BEFORE audio starts: if the device hangs in
   * the first PicoFrame, the diag file ends at 'warmup f0...' and we know.
   * Boot-time SD write, allowed; the file is sealed before the main loop. */
  diag_log("warmup f0 (32X engages lazily when the 68K writes ADEN)...\n");
  PicoIn.pad[0] = 0;
  PicoFrame();
  {
    uint16_t *fb = lcd_get_active_buffer();
    uint32_t nz = 0;
    for (int i = 0; i < 320 * 240; i++) nz |= fb[i];
    diag_log("warmup f0 done: AHW=%x fb_nonblank=%d\n",
             (unsigned)PicoIn.AHW, nz != 0);
  }
  audio_start_playing(MD32X_AUDIO_RATE / md32x_fps);
  diag_log("entering main loop (fps=%d, diag sealed - no more SD writes)\n", md32x_fps);
  md32x_diag_sealed = true;

  if (load_state)
    odroid_system_emu_load_state(save_slot);
  else
    lcd_clear_buffers();

  while (1) {
    wdog_refresh();

    bool drawFrame = common_emu_frame_loop();

    odroid_input_read_gamepad(&joystick);
    common_emu_input_loop(&joystick, options, NULL);
    common_emu_input_loop_handle_turbo(&joystick);

    PicoIn.pad[0] = read_md_pad(&joystick);

    if (drawFrame) set_out_buffer();
    /* skip_frame tells picodrive to run emulation but not rasterize */
    PicoIn.skipFrame = drawFrame ? 0 : 1;
    PicoFrame();

    if (drawFrame) {
      common_ingame_overlay();
      lcd_swap();
    }

    common_emu_sound_sync(false);
  }
}
