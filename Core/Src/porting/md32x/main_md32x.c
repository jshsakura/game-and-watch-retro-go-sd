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
#include "md32x_border_clear.h"

#include "pico/pico_types.h"   /* s8/s16/s32 — MUST precede pico.h */
#include "pico/pico.h"
#include "pico/pico_int.h"     /* Pico.est.Draw2FB binding (Draw2 shim below) */
#include "pico/state.h"

static void diag_log(const char *fmt, ...);   /* boot diag, defined below */

/* ---- geometry / rates ----------------------------------------------------- */
#define MD32X_FPS            60
#define MD32X_WIDTH          320          /* 32X is H40 (320) most of the time  */
#define MD32X_HEIGHT         224
#define MD32X_TOP_MARGIN     ((240 - MD32X_HEIGHT) / 2)   /* 8 lines            */
#define MD32X_AUDIO_RATE     44100
#define MD32X_AUDIO_MAX      (MD32X_AUDIO_RATE / 50 + 16) /* worst-case/frame   */

/* Savestate stamp: refuse a file this build did not write (project rule). The
 * payload itself is picodrive's own versioned PicoState stream.
 * V2: lazy-T decomposed the SH2 T-bit into sh2->t_flag (SH2_REG_SIZE 92->100),
 *     shifting the extra pack fields (pending_int_irq/vector/m68krcycles_done)
 *     by 8 bytes. V1 savestates would misparse and corrupt CPU state. */
#define MD32X_STATE_MAGIC    0x4D583258u  /* "MX2X" */
#define MD32X_STATE_VERSION  2

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
 * shrinks (the m68k bank no longer lives there, see below).
 * MD32X_D2FB_LINE/BYTES now live in main_md32x.h (shared with
 * md32x_profile.c's AHB budget _Static_assert, 0720 overflow fix). */
static uint8_t *md32x_draw2fb;

void PicoDraw2SetOutBuf(void *dest, int increment) {
  /* draw2.c's binding, verbatim minus the renderer (cf. rig_32x_draw2fb.c).
   * References to Pico resolve to md32x__Pico — the redefine pass renames
   * references in this object too, so this binds the overlay's own state. */
  if (dest) {
    Pico.est.Draw2FB = dest;
    Pico.est.Draw2Width = increment;
  } else {
    if (md32x_draw2fb == NULL) {
      /* ahb_only_malloc does NOT return NULL on pool overflow -- it hits an
       * assert (0720 device Hardfault triage: assert() is live here, this
       * TU isn't built with NDEBUG). Log headroom BEFORE the call so the
       * number survives even if the call itself never returns. */
      extern size_t ahb_get_free_size(void);
      diag_log("draw2fb: ahb_free_before=%u need=%u\n",
               (unsigned)ahb_get_free_size(), (unsigned)MD32X_D2FB_BYTES);
      md32x_draw2fb = (uint8_t *)ahb_calloc(1, MD32X_D2FB_BYTES);
      diag_log("draw2fb: alloc=%p ahb_free_after=%u\n",
               (void *)md32x_draw2fb, (unsigned)ahb_get_free_size());
    }
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
/* Buffer stays at the framebuffer ORIGIN, full 320x240 pitch. picodrive
 * CENTERS content itself (V28 renders at rows 8..231; 32X is always 320
 * wide) and emu_video_mode_change only REPORTS the content rect — the
 * libretro frontend crops, it never re-points the buffer. The first device
 * build added its own margin on top: top band doubled to 16 rows and the
 * bottom 8 content rows were written PAST the framebuffer (into the other
 * buffer's head). */
static void set_out_buffer(void) {
  PicoDrawSetOutBuf(lcd_get_active_buffer(), 320 * 2);
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
  (void)start_col; (void)col_count;   /* 32X: always full 320 wide, see md32x_border_clear.c */
  md32x_border_clear_set_content_rect(start_line, line_count);
  /* Mode changes are rare; wipe the ACTIVE buffer so stale borders don't
   * linger.  Previously this called lcd_clear_buffers() (both buffers), which
   * has NO lcd_sleep_while_swap_pending() guard — the write-buffered clear
   * overtook the scanout beam and produced a bottom black band (the exact
   * race lcd_clear_active_buffer() was written to prevent).  The inactive
   * buffer is overwritten on the next lcd_swap() anyway, so clearing only
   * the active buffer is sufficient and race-free. */
  lcd_clear_active_buffer();
  set_out_buffer();
}

/* ---- savestate (M2) --------------------------------------------------------
 * PicoStateFP keeps our project stamp and picodrive's versioned chunk stream in
 * one file without reopening/truncating it. These callbacks deliberately match
 * picodrive's area I/O ABI instead of casting the libc function pointers. */
static size_t md32x_state_read(void *ptr, size_t size, size_t count, void *file) {
  return fread(ptr, size, count, (FILE *)file);
}

static size_t md32x_state_write(void *ptr, size_t size, size_t count, void *file) {
  return fwrite(ptr, size, count, (FILE *)file);
}

static size_t md32x_state_eof(void *file) {
  return (size_t)feof((FILE *)file);
}

static int md32x_state_seek(void *file, long offset, int whence) {
  return fseek((FILE *)file, offset, whence);
}

static bool md32x_SaveState(const char *pathName) {
  const uint32_t header[2] = { MD32X_STATE_MAGIC, MD32X_STATE_VERSION };
  FILE *file = fopen(pathName, "wb");
  if (file == NULL)
    return false;

  bool ok = fwrite(header, sizeof(header), 1, file) == 1 &&
            PicoStateFP(file, 1, md32x_state_read, md32x_state_write,
                        md32x_state_eof, md32x_state_seek) == 0;
  if (fclose(file) != 0)
    ok = false;
  if (!ok)
    remove(pathName);
  return ok;
}

static bool md32x_LoadState(const char *pathName) {
  uint32_t header[2];
  FILE *file = fopen(pathName, "rb");
  if (file == NULL)
    return false;

  bool ok = fread(header, sizeof(header), 1, file) == 1 &&
            header[0] == MD32X_STATE_MAGIC &&
            header[1] == MD32X_STATE_VERSION &&
            PicoStateFP(file, 0, md32x_state_read, md32x_state_write,
                        md32x_state_eof, md32x_state_seek) == 0;
  fclose(file);
  if (ok)
    set_out_buffer();
  return ok;
}

static void *md32x_Screenshot(void) {
  lcd_wait_for_vblank();
  return lcd_get_active_buffer();
}

/* ---- cart SRAM (M2) --------------------------------------------------------
 * PicoStateFP saves CPU/memory state into versioned slot files but does NOT
 * save cart save-RAM — that lives in a separate .sram file, persisted across
 * sessions (not per-slot), exactly like every other core in this project.
 * Pico.sv.data holds both plain SRAM and EEPROM-backed data; Pico.sv.size is
 * 0 for carts with no battery.  We skip the write when no SRAM is allocated
 * or the data is all-zero (matches libretro's retro_get_memory_size rule so
 * we don't sprinkle empty .sram files for games that never wrote any). */
static void md32x_SramSave(void) {
  if (Pico.sv.data == NULL || Pico.sv.size == 0)
    return;
  /* quick all-zero check: don't create a file for a game that never wrote */
  unsigned int sum = 0;
  for (unsigned int i = 0; i < Pico.sv.size; i++)
    sum |= Pico.sv.data[i];
  if (sum == 0)
    return;

  char *sram_path = odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ACTIVE_FILE->path);
  if (sram_path == NULL) return;
  FILE *file = fopen(sram_path, "wb");
  if (file) {
    size_t n = fwrite(Pico.sv.data, 1, Pico.sv.size, file);
    (void)n;
    fclose(file);
  }
  free(sram_path);
}

static void md32x_SramLoad(void) {
  if (Pico.sv.data == NULL || Pico.sv.size == 0)
    return;
  char *sram_path = odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ACTIVE_FILE->path);
  if (sram_path == NULL) return;
  FILE *file = fopen(sram_path, "rb");
  if (file) {
    size_t n = fread(Pico.sv.data, 1, Pico.sv.size, file);
    (void)n;
    fclose(file);
  }
  free(sram_path);
}

/* ---- sleep wake-up (M2) ----------------------------------------------------
 * gw_sleep() restores the *settings* OC level on wake, but 32X forces the
 * scoped maximum boost during gameplay (common_emu_auto_oc(1) in app_main).
 * Re-apply the boost and reinit audio (SystemClock_Config also reprograms
 * the audio PLL).  Mirrors the gwenesis pattern. */
static void md32x_SleepWakeUp(void) {
  common_emu_auto_oc(1);
  odroid_audio_init(odroid_audio_sample_rate_get());
  audio_start_playing_full_length(audio_get_buffer_full_length());
  set_out_buffer();
}

/* Pause/menu repaint. The pause banner calls this through the pointer given
 * to common_emu_input_loop — a NULL there was the device's PC=0 Hardfault
 * (LR = odroid_overlay_sleep_pause_banner; the C64-era rule: a custom-loop
 * core MUST pass a non-NULL repaint). We can't cheaply re-render a picodrive
 * frame on demand, so copy the SHOWN frame into the active buffer and draw
 * the overlay on top.
 *
 * The overlay's _repaint() clears the active buffer, calls us, then lcd_swap()s.
 * After the first swap the DISPLAYED buffer holds the menu composite (game +
 * darken + dialog), not the pure game frame.  A naive "copy displayed into
 * active" would therefore smear the previous menu state onto every subsequent
 * repaint.  We freeze the game-frame pointer on the FIRST call (when the
 * displayed buffer is still the pure game frame) and keep copying from THAT
 * frozen buffer for the lifetime of this menu session.  The flag resets when
 * the main loop renders a fresh frame (md32x_repaint_reset). */
static int md32x_repaint_first = 1;

void md32x_repaint_reset(void) { md32x_repaint_first = 1; }

static void md32x_repaint(void) {
  md32x_border_clear_notify_menu_open();
  uint16_t *active = lcd_get_active_buffer();
  static uint16_t *frozen;
  if (md32x_repaint_first) {
    /* displayed (inactive) buffer still holds the pure game frame */
    frozen = (active == (uint16_t *)framebuffer1)
                 ? (uint16_t *)framebuffer2 : (uint16_t *)framebuffer1;
    md32x_repaint_first = 0;
  }
  /* After lcd_swap toggles the double buffer, frozen aliases the *same*
   * physical FB as active.  A self-memcpy is a no-op (harmless) but we
   * skip it to stay explicit.  The overlay _repaint no longer pre-clears
   * the active buffer for NO_BG_DARKEN callers (us), so frozen is never
   * wiped by lcd_clear_active_buffer — the original 92425edd bug. */
  if (frozen && frozen != active)
    memcpy(active, frozen, 320 * 240 * sizeof(uint16_t));
  common_ingame_overlay();
}

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
    diag_log("FATAL: 32x.xip size %u != firmware's %u (stale xip? re-copy /cores)\n",
             (unsigned)g_xip_size, (unsigned)expected);
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
/* 1536 -> 1024 (0725) -> 768 (0726): the MD32X overlay BSS sits flush
 * against __RAM_EMU_END__ in the MD32X_DEVICE_PROFILE build and this buffer
 * is the only slack left (0725 gave back 512 B for the pcwall probe; 0726
 * the BT/S frame-wait fold's .text cost another 152 B). A full good boot
 * writes ~700 B of breadcrumbs (15 lines, longest is rom=<path>); a very
 * long rom path can now truncate the last line or two of an already-good
 * boot — the failure lines diag exists for come long before the tail. */
static char md32x_diag[768];
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

/* ---- Device-side DWT phase profiler ----------------------------------------
 * Enabled only with MD32X_DEVICE_PROFILE=1. All storage, the DWT read, and
 * the recording/dump logic live in md32x_profile.c — kept in a separate TU
 * so its .text/.rodata land in .xip_md32x (QSPI flash) via the linker
 * script's `build/md32x/*.o` sweep, NOT in this file's overlay RAM
 * (main_md32x.o's .text is explicitly forced into overlay by the linker
 * script — an earlier inline version of this profiler, built independently
 * on a parallel branch and reconciled here 0720, overflowed MD32X BSS by
 * 2088B for exactly this reason: its qsort+percentile+fprintf-heavy dump
 * function counted as RAM_EMU code, not just its ~300B of actual state).
 * Only that small state (pp_counters/refcounts/prof_sum_* — the big
 * per-frame delta pools are AHB-allocated, see md32x_profile.c) stays in
 * this file's overlay BSS. See md32x_profile.c for the full design. */
#ifdef MD32X_DEVICE_PROFILE
#include "md32x_profile.h"
#endif

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

  /* Draw every frame this core emulates.
   *
   * The shared overload guard drew one in four, and on this core that was three
   * quarters of the player's frames thrown away for nothing. Frameskip buys
   * back the cost of drawing, and here drawing is 1.9% of the frame -- the
   * other 98% is two SH-2s and a 68000 that no amount of skipping makes
   * cheaper. Nor is there a real-time deadline to protect: the core runs at
   * roughly a third of console speed whatever it does.
   *
   * Measured on hardware, cold-boot demo scene, two samples per arm, drawn fps
   * against emulated fps:
   *
   *   Doom          1-in-4  19.9 emu /  4.98 drawn      1-in-1  18.3 / 18.33
   *   Chaotix       1-in-4  15.4 emu /  3.87 drawn      1-in-1  13.1 / 13.12
   *   Kolibri       1-in-4   9.9 emu /  2.48 drawn      1-in-1   9.4 /  9.40
   *
   * i.e. 3.4x to 3.8x the visible frames for 5-15% of emulated fps. */
  common_emu_set_forced_draw_ratio(1);

  /* Spare-RAM allocator base (gwenesis pattern). ahb_calloc() tries ram_malloc
   * FIRST and ram_malloc asserts on ram_start==0 — first device boot died
   * exactly there when gnw_m68k_bank_alloc() ran. The overlay margin (~9K) is
   * too small for the 64K bank, so it falls through to AHB as intended. */
  extern void *_OVERLAY_MD32X_BSS_END[];
  ram_start = (uint32_t)&_OVERLAY_MD32X_BSS_END;

  odroid_system_init(APPID_32X, MD32X_AUDIO_RATE);
  odroid_system_emu_init(&md32x_LoadState, &md32x_SaveState, &md32x_Screenshot,
                         NULL, &md32x_SleepWakeUp, &md32x_SramSave, NULL);
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
  diag_log("rom cached: addr=%p len=%u head=%02x%02x%02x%02x\n",
           (void *)rom, (unsigned)sz,
           rom ? rom[0] : 0, rom ? rom[1] : 0, rom ? rom[2] : 0, rom ? rom[3] : 0);

  /* XIP cold code+rodata MUST be cached and the overlay's sentinel pointers
   * patched BEFORE the first picodrive call — PicoInit itself lives in XIP. */
  if (!Md32xCacheXipToFlash()) {
    diag_log("FATAL: xip cache failed (missing %s?)\n", MD32X_XIP_PATH);
    odroid_overlay_alert("Missing " MD32X_XIP_PATH " - re-run the update");
    odroid_system_switch_app(0);
    return;
  }
  diag_log("xip cached: addr=%p size=%u off=%d\n",
           (void *)g_xip_addr, (unsigned)g_xip_size, (int)g_xip_offset);
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

  /* No SH-2 BRA-self idle-skip whitelist here. It was gated by a full-ROM
   * CRC32 (one dump only, every variant/region falls through) AND it
   * measured 0 device fps effect (docs/32X_PERFORMANCE_RESULTS.md 측정10 —
   * QEMU rig instruction-count savings didn't translate to device cycles)
   * AND it broke Doom's gunshot PWM SFX: the whitelisted BRA-self spin was
   * the SH-2 code path leading into the sound-effect trigger, so scheduler
   * SLEEP was skipping state a game-code-driven pattern match wouldn't have
   * (state-exact != cycle-exact, the WS idle-skip lesson). Net: pure loss,
   * removed. gnw_sh2_idle_skip stays at its compiled-in default (0 — see
   * sh2pico.c GNW_SH2_IDLE_SKIP_DEFAULT). If this class of optimization
   * returns, it must key off an opcode-pattern fingerprint (like SegaCD's
   * poll detector) — never a whole-ROM CRC. */

  diag_log("PicoLoadMedia: mt=%d AHW=%x romsize=%u pal=%d\n",
           (int)mt, (unsigned)PicoIn.AHW, (unsigned)Pico.romsize,
           (int)Pico.m.pal);

  /* Load cart SRAM (battery save) before the first frame.  PicoLoadMedia ->
   * PicoCartInsert allocates Pico.sv.data for carts that have SRAM/EEPROM;
   * carts without it have sv.size==0 and SramLoad is a no-op. */
  md32x_SramLoad();
  diag_log("sram: size=%u flags=%02x\n",
           (unsigned)Pico.sv.size, (unsigned)Pico.sv.flags);

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

#ifdef MD32X_DEVICE_PROFILE
  /* Arm the DWT cycle counter and allocate the delta pools AFTER the diag file
   * is sealed, so neither interferes with boot-time SD writes. The counter
   * then runs free for the whole main loop; per-frame deltas are safe (wrap
   * ≈ 12.6 s @ 340 MHz). Pools come from AHB SRAM (see MEMORY PLACEMENT above). */
  md32x_profile_init();
  common_emu_enable_dwt_cycles();
#endif

  if (load_state)
    odroid_system_emu_load_state(save_slot);
  else
    lcd_clear_buffers();

  while (1) {
    wdog_refresh();

#ifdef GNW_AUTOSAVE_FRAME
    /* Measurement-only: write slot 0 once, N frames in, then carry on. Copied
     * from the SNES core, for the same reason it exists there -- an A/B needs a
     * scene both arms see identically, and nobody can play a console with a
     * debug probe soldered to it, so the console makes its own scene.
     *
     * On this core the scene that matters is the attract demo: Doom's title
     * screen is a still image and every 32X number in docs/32X_CLOSED.md that
     * came from a boot anchor was measured on one. Pick N past the title.
     *
     * One-shot SD write at a known frame, through the same call the menu uses.
     * Never enabled in a shipping build. */
    {
      static bool autosaved = false;
      static uint32_t autosave_frames = 0;
      if (!autosaved && ++autosave_frames >= (uint32_t)GNW_AUTOSAVE_FRAME) {
        autosaved = true;
        odroid_audio_mute(true);
        printf("md32x: autosave slot 0 at frame %lu -> %d\n",
               (unsigned long)autosave_frames,
               (int)odroid_system_emu_save_state(0));
        odroid_audio_mute(false);
        common_emu_state.startup_frames = 0;
      }
    }
#endif

#ifdef MD32X_DEVICE_PROFILE
    /* Single DWT clear for the whole iteration. Cumulative reads at each phase
     * boundary below give EXACTLY disjoint deltas (tama pattern): the five
     * phase buckets never overlap, and the final read is loop_total. No nested
     * intervals, no double-counting. */
    common_emu_clear_dwt_cycles();
#endif

    bool drawFrame = common_emu_frame_loop();

#ifdef MD32X_DEVICE_PROFILE
    uint32_t t_pace = common_emu_get_dwt_cycles();   /* after pace, before proc */
#endif

    odroid_input_read_gamepad(&joystick);
    common_emu_input_loop(&joystick, options, &md32x_repaint);
    common_emu_input_loop_handle_turbo(&joystick);

    /* md32x_border_clear.h: fixes border-row flicker after closing the
     * overlay. Must see the SAME drawFrame value used below to decide
     * set_out_buffer()/lcd_swap(), or its two-buffers guarantee breaks. */
    md32x_border_clear_tick(drawFrame);

    PicoIn.pad[0] = read_md_pad(&joystick);

    if (drawFrame) set_out_buffer();
    /* skip_frame tells picodrive to run emulation but not rasterize */
    PicoIn.skipFrame = drawFrame ? 0 : 1;

#ifdef MD32X_DEVICE_PROFILE
    uint32_t t_proc = common_emu_get_dwt_cycles();   /* after proc, before pico */
#endif

    PicoFrame();

#ifdef MD32X_DEVICE_PROFILE
    uint32_t t_pico = common_emu_get_dwt_cycles();   /* after pico, before blit */
#endif

    if (drawFrame) {
      common_ingame_overlay();
      lcd_swap();
      /* A fresh game frame is now on display — next menu open should freeze
       * THIS frame as the menu background, not a stale menu composite. */
      md32x_repaint_reset();
    }

#ifdef MD32X_DEVICE_PROFILE
    uint32_t t_blit = common_emu_get_dwt_cycles();   /* after blit, before audio */
#endif

    common_emu_sound_sync(false);

#ifdef MD32X_DEVICE_PROFILE
    uint32_t t_audio = common_emu_get_dwt_cycles();  /* after audio == loop_total */
    md32x_profile_record(drawFrame, t_pace, t_proc, t_pico, t_blit, t_audio);
#endif
  }
}
