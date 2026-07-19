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
#include "pico/state.h"

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
  (void)start_line; (void)line_count; (void)start_col; (void)col_count;
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

/* ---- device-side DWT performance profile (MD32X_DEVICE_PROFILE only) --------
 *
 * PURPOSE: verify that a QEMU-rig relative win (e.g. Metal Head −51.3%) shows
 * up as an ABSOLUTE fps/cycle improvement on the real STM32H7. QEMU instruction
 * counts are not device cycle counts; only DWT settles the question (see
 * docs/32X_PERFORMANCE_HISTOGRAM_GUIDE.md §8).
 *
 * BYTE-IDENTICAL GUARANTEE: every line in this section is compiled ONLY when
 * -DMD32X_DEVICE_PROFILE is passed on the compile line. The default release
 * build never sees it — the preprocessor strips the whole block — so
 * build/md32x/main_md32x.o is code-identical with and without the flag (verify
 * by diffing the objdump -d output; only debug-info line numbers shift).
 *
 * SD-WRITE-ONCE RULE: profiling data is accumulated in RAM and written to
 * /32x_dwt.txt EXACTLY ONCE, after MD32X_PROFILE_FRAMES frames. Reopening the
 * file mid-emulation would distort the very pacing we measure and risks card
 * corruption (the /32x_diag.txt saga above). A `prof_dumped` flag makes the
 * write strictly one-shot; after it fires, recording stops forever.
 *
 * BUCKET MEANING (disjoint — see main-loop instrumentation for the clear/get
 * sequence):
 *   pace  : common_emu_frame_loop()              — frame pacing / wait
 *   proc  : input + pad read + out-buffer setup  — front-end, before PicoFrame
 *   pico  : PicoFrame()                          — the emulation (heaviest)
 *   blit  : common_ingame_overlay() + lcd_swap() — drawn frames only (skip → ~0)
 *   audio : common_emu_sound_sync(false)         — audio submit/sync
 *   total : one whole loop iteration             — over-budget gate uses this
 *
 * The five phase buckets are EXACTLY disjoint: a single DWT clear at the top of
 * each loop iteration, then cumulative reads at every phase boundary; disjoint
 * deltas are the differences between consecutive reads (the proven pattern in
 * Core/Src/porting/tama/main_tama.c). `total` is the final cumulative read, so
 * it equals the sum of the five phases plus negligible inter-phase branch cost.
 * No nested or overlapping intervals are ever summed. The DWT helpers used here
 * (common_emu_enable/clear/get_dwt_cycles) live in Core/Src/porting/common.c;
 * no DWT register is touched directly here.
 *
 * MEMORY PLACEMENT: the md32x overlay is at ~99.8 % of RAM_EMU (only ~1.6 KB of
 * BSS headroom), so the 28.8 KB of per-frame delta pools CANNOT be static —
 * they would blow the link ASSERT. They are ahb_calloc'd from the SEPARATE
 * 120 KB AHB SRAM pool (the draw2fb buffer already takes ~82 KB of it, leaving
 * ~38 KB — plenty for two 14.4 KB pools). If the allocation fails, profiling
 * is silently inert (prof_active = false). Only the tiny accumulators/counters
 * below are static (~120 B of overlay BSS, well within headroom).
 */
#ifdef MD32X_DEVICE_PROFILE
#define MD32X_PROFILE_FRAMES  600u   /* ~10 s @60 fps / ~12 s @50 fps; then dump */
#define MD32X_PROF_PATH       "/32x_dwt.txt"

enum {
  PROF_BUCK_PACE = 0,   /* common_emu_frame_loop()                    */
  PROF_BUCK_PROC,       /* input/pad/out-buffer before PicoFrame      */
  PROF_BUCK_PICO,       /* PicoFrame() — the emulation                */
  PROF_BUCK_BLIT,       /* overlay + lcd_swap (drawn only; skip ≈ 0)  */
  PROF_BUCK_AUDIO,      /* common_emu_sound_sync(false)               */
  PROF_BUCK_TOTAL,      /* whole loop iteration                       */
  PROF_BUCK_COUNT
};

/* Per-frame 32-bit DWT delta pools — AHB-allocated (see MEMORY PLACEMENT above).
 * Separate drawn/skip pools so render cost does not average into compute-only
 * skips. A per-frame delta at 340 MHz fits easily in 32 bits (60 fps budget is
 * ~5.7 M cycles; the raw counter wraps only every ~12.6 s). */
static uint32_t *prof_delta_drawn;  /* [PROF_BUCK_COUNT * MD32X_PROFILE_FRAMES] */
static uint32_t *prof_delta_skip;   /* [PROF_BUCK_COUNT * MD32X_PROFILE_FRAMES] */
/* uint64 accumulators: 600 hot pico frames can sum past 2^32, so accumulate
 * the per-frame 32-bit deltas in 64 bits even though each sample is 32-bit. */
static uint64_t prof_sum_drawn[PROF_BUCK_COUNT];
static uint64_t prof_sum_skip[PROF_BUCK_COUNT];
static uint32_t prof_drawn_count;
static uint32_t prof_skip_count;
static bool prof_dumped;    /* strictly one-shot SD write  */
static bool prof_active;    /* false if AHB alloc failed   */

/* flat index into the AHB pools: bucket-major so each bucket's frames are
 * contiguous (qsort in the dump operates on a bucket's slice in place). */
#define PROF_AT(pool, bucket, i)  ((pool)[(bucket) * MD32X_PROFILE_FRAMES + (i)])

/* ---- PicoFrame() sub-phase breakdown (rides picodrive's pprof probes) ----
 *
 * The PROF_BUCK_PICO bucket above says how much of the frame is PicoFrame().
 * It does not say WHICH part dominates: master SH-2, slave SH-2, 68K, MD VDP
 * draw, 32X compositor draw (Draw2FB/FinalizeLine32x), or FM/PWM mixing.
 * picodrive already brackets every one of these with pprof_start/pprof_end
 * (external/picodrive/platform/linux/pprof.h and the call sites in
 * pico/32x/32x.c, pico/draw.c, pico/sound/sound.c) — normally live only for
 * the QEMU rig's RIG_PHASE_PROF build. Defining MD32X_DEVICE_PROFILE routes
 * the SAME probes to DWT_CYCCNT (md32x_dwt_now() below) instead of the rig's
 * icount timer, so the exact disjoint accounting the rig used to rank ROMs
 * (docs/32X_PERFORMANCE_RESULTS.md) now runs on real hardware.
 *
 * Storage: pico_int.h declares `pp_counters`/`refcounts` extern; something
 * has to define them once the firmware links picodrive in, same as the QEMU
 * rig does in tools/m7_qemu_rig/rig_32x.c. Sum-only over the whole profiling
 * window (no percentile pools) — the open question is which phase dominates
 * PicoFrame() on average, not its frame-to-frame variance.
 */
static struct pp_counters s_pp_counters;
struct pp_counters *pp_counters = &s_pp_counters;
static int s_pp_refcounts[pp_total_points];
int *refcounts = s_pp_refcounts;

unsigned int md32x_dwt_now(void) { return common_emu_get_dwt_cycles(); }

/* Allocate the two delta pools from AHB SRAM. Called once before the main
 * loop. On failure, prof_active stays false and profiling is silently inert. */
static void md32x_profile_init(void) {
  prof_delta_drawn = ahb_calloc((size_t)PROF_BUCK_COUNT * MD32X_PROFILE_FRAMES,
                                sizeof(uint32_t));
  prof_delta_skip  = ahb_calloc((size_t)PROF_BUCK_COUNT * MD32X_PROFILE_FRAMES,
                                sizeof(uint32_t));
  prof_active = (prof_delta_drawn != NULL && prof_delta_skip != NULL);
}

static int prof_u32_cmp(const void *a, const void *b) {
  uint32_t ua = *(const uint32_t *)a, ub = *(const uint32_t *)b;
  return (ua > ub) - (ua < ub);
}

/* nearest-rank percentile from an ALREADY SORTED array of n elements (p:0..100) */
static uint32_t prof_pct_sorted(const uint32_t *sorted, uint32_t n, uint32_t p) {
  if (n == 0) return 0;
  uint32_t idx = (n * p) / 100u;
  if (idx >= n) idx = n - 1u;
  return sorted[idx];
}

/* Emit one bucket line to an open file. Sorts the per-frame delta slice IN
 * PLACE — safe because data is dead after the single dump. kind 0=drawn,1=skip. */
static void prof_emit_bucket(FILE *f, const char *label, uint32_t kind,
                              uint32_t *pool, uint32_t n, uint64_t sum) {
  if (n == 0) {
    fprintf(f, "  %-6s[%c]: n=0\n", label, kind ? 'S' : 'D');
    return;
  }
  qsort(pool, n, sizeof(uint32_t), prof_u32_cmp);
  fprintf(f, "  %-6s[%c]: n=%u avg=%llu p50=%llu p90=%llu p95=%llu p99=%llu\n",
          label, kind ? 'S' : 'D', (unsigned)n,
          (unsigned long long)(sum / n),
          (unsigned long long)prof_pct_sorted(pool, n, 50),
          (unsigned long long)prof_pct_sorted(pool, n, 90),
          (unsigned long long)prof_pct_sorted(pool, n, 95),
          (unsigned long long)prof_pct_sorted(pool, n, 99));
}

/* One controlled dump to /32x_dwt.txt: mute audio, open/write/close once,
 * refresh the watchdog around each slow step, unmute. Never called more than
 * once (guarded by prof_dumped at the call site). */
static void md32x_profile_dump(void) {
  extern uint32_t SystemCoreClock;   /* system_stm32h7xx.c */

  /* Budget: cycles available per frame at the current clock and region.
   *   frame_time_10us is in 10-us ticks; SystemCoreClock/100000 converts a
   *   10-us tick to cycles (10 us = 1e-4 s; clk * 1e-4 = clk/100000). */
  uint64_t budget = (uint64_t)common_emu_state.frame_time_10us
                    * (uint64_t)(SystemCoreClock / 100000u);

  /* Over-budget frames (loop_total > budget) for drawn and skip separately. */
  uint32_t over_drawn = 0, over_skip = 0;
  uint32_t *total_drawn = prof_delta_drawn + PROF_BUCK_TOTAL * MD32X_PROFILE_FRAMES;
  uint32_t *total_skip  = prof_delta_skip  + PROF_BUCK_TOTAL * MD32X_PROFILE_FRAMES;
  for (uint32_t i = 0; i < prof_drawn_count; i++)
    if (total_drawn[i] > budget) over_drawn++;
  for (uint32_t i = 0; i < prof_skip_count; i++)
    if (total_skip[i] > budget) over_skip++;

  /* Mute so the dump does not buzz; harmless if already muted. */
  odroid_audio_mute(true);
  wdog_refresh();

  FILE *f = fopen(MD32X_PROF_PATH, "wb");
  wdog_refresh();
  if (f == NULL) { odroid_audio_mute(false); return; }

  fprintf(f, "=== 32X DWT device profile ===\n");
  fprintf(f, "build: MD32X_DEVICE_PROFILE=1 (opt switch ON)\n");
  /* No firmware-commit symbol is exported by this build (no _build_version /
   * git-rev global); the file path + opt-switch line identify the run. */
  fprintf(f, "clk=%lu Hz  region=%s  oc_user=%u (common_emu_auto_oc floor=1)\n",
          (unsigned long)SystemCoreClock,
          Pico.m.pal ? "PAL" : "NTSC",
          (unsigned)odroid_settings_cpu_oc_level_get());
  fprintf(f, "frame_budget=%llu cycles  frames_drawn=%u frames_skip=%u  total=%u\n",
          (unsigned long long)budget,
          (unsigned)prof_drawn_count, (unsigned)prof_skip_count,
          (unsigned)(prof_drawn_count + prof_skip_count));
  fprintf(f, "over_budget_total(drawn)=%u/%u  over_budget_total(skip)=%u/%u\n",
          (unsigned)over_drawn, (unsigned)prof_drawn_count,
          (unsigned)over_skip, (unsigned)prof_skip_count);
  fprintf(f, "buckets (D=drawn, S=skip; values are DWT cycles):\n");
  wdog_refresh();

  static const char *const names[PROF_BUCK_COUNT] = {
    "pace", "proc", "pico", "blit", "audio", "total"
  };
  for (uint32_t b = 0; b < PROF_BUCK_COUNT; b++) {
    prof_emit_bucket(f, names[b], 0,
                     prof_delta_drawn + b * MD32X_PROFILE_FRAMES,
                     prof_drawn_count, prof_sum_drawn[b]);
    prof_emit_bucket(f, names[b], 1,
                     prof_delta_skip + b * MD32X_PROFILE_FRAMES,
                     prof_skip_count, prof_sum_skip[b]);
  }

  /* PicoFrame() sub-phase breakdown (picodrive pprof probes, sum-only —
   * see the block comment above pp_counters/refcounts). pico_total is the
   * SAME quantity PROF_BUCK_PICO measured from the outside; it is printed
   * so the two can be cross-checked against each other. Any nonzero
   * refcount below means a pprof_start/pprof_end pair leaked (should never
   * happen — see the widened gates in draw.c/32x.c/sound.c) and the sums
   * above it are suspect. */
  {
    uint32_t total_frames = prof_drawn_count + prof_skip_count;
    uint64_t pico_total = prof_sum_drawn[PROF_BUCK_PICO] + prof_sum_skip[PROF_BUCK_PICO];
    static const struct { int point; const char *label; } phases[] = {
      { pp_frame,   "frame"   },  /* whole PicoFrame() — cross-check vs pico above */
      { pp_msh2,    "msh2"    },  /* master SH-2 interpreter                       */
      { pp_ssh2,    "ssh2"    },  /* slave SH-2 interpreter                        */
      { pp_m68k,    "m68k"    },  /* 68000 interpreter                            */
      { pp_draw,    "draw_md" },  /* MD VDP line render (pico/draw.c)             */
      { pp_draw32x, "draw32x" },  /* 32X compositor layer merge (pico/32x/draw.c) */
      { pp_sound,   "sound"   },  /* PSG/mix, excludes fm/pwm paused sub-windows   */
      { pp_fm,      "fm"      },  /* YM2612 render                                */
      { pp_pwm,     "pwm"     },  /* 32X PWM chip render                          */
    };
    int leaked = 0;
    for (uint32_t i = 0; i < ARRAY_SIZE(phases); i++)
      if (s_pp_refcounts[phases[i].point] != 0) leaked++;

    fprintf(f, "picoframe sub-phases (sum over whole window, refcount_leaks=%d):\n",
            leaked);
    fprintf(f, "  pico_total(outside)=%llu  frame_total(pprof)=%llu\n",
            (unsigned long long)pico_total,
            (unsigned long long)s_pp_counters.counter[pp_frame]);
    for (uint32_t i = 0; i < ARRAY_SIZE(phases); i++) {
      uint64_t sum = (uint64_t)s_pp_counters.counter[phases[i].point];
      uint64_t avg = total_frames ? sum / total_frames : 0;
      unsigned pct_x10 = pico_total ? (unsigned)((sum * 1000) / pico_total) : 0;
      fprintf(f, "  %-8s: sum=%llu avg/frame=%llu pct_of_pico=%u.%u%%\n",
              phases[i].label, (unsigned long long)sum,
              (unsigned long long)avg, pct_x10 / 10, pct_x10 % 10);
    }
  }

  wdog_refresh();
  fclose(f);
  wdog_refresh();
  odroid_audio_mute(false);
}
#endif /* MD32X_DEVICE_PROFILE */

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
                         NULL, &md32x_SleepWakeUp, &md32x_SramSave);
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

  /* SH-2 BRA-self idle-skip whitelist: only verified ROMs get scheduler
   * SLEEP on BRA-self (saves ~99% of sleeping SH-2 cost). Unverified
   * ROMs use the safe icount-burn path. CRC32 computed from raw ROM
   * data (pre-byteswap). Add verified CRCs here as games are tested. */
  extern int gnw_sh2_idle_skip;
  {
    uint32_t crc = 0xFFFFFFFF;
    for (unsigned i = 0; i < sz; i++) {
      crc ^= rom[i];
      for (int b = 0; b < 8; b++)
        crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
      if ((i & 0xFFFFu) == 0) wdog_refresh();   /* 3 MB CRC over slow QSPI XIP
                                                   overruns the ~472 ms watchdog
                                                   window → reset (device 즉사) */
    }
    crc ^= 0xFFFFFFFF;
    /* Doom 32X (US): slave SH-2 verified sleeping safely */
    gnw_sh2_idle_skip = (crc == 0xb0239812u) ? 1 : 0;
    diag_log("idle_skip: crc=%08x -> %s\n", (unsigned)crc,
             gnw_sh2_idle_skip ? "ON" : "OFF");
  }

  diag_log("PicoLoadMedia: mt=%d AHW=%x romsize=%lu pal=%d\n",
           (int)mt, (unsigned)PicoIn.AHW, (unsigned long)Pico.romsize,
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

    /* Record per-frame disjoint deltas into the drawn or skip pool, then
     * one-shot dump when the window fills. After the dump, recording stops
     * forever (prof_dumped) so steady-state play is never disturbed. If the
     * AHB allocation failed at init (prof_active false), skip entirely. */
    if (prof_active && !prof_dumped) {
      uint32_t total = prof_drawn_count + prof_skip_count;
      if (total < MD32X_PROFILE_FRAMES) {
        uint32_t delta[PROF_BUCK_COUNT];
        delta[PROF_BUCK_PACE]  = t_pace;
        delta[PROF_BUCK_PROC]  = t_proc - t_pace;
        delta[PROF_BUCK_PICO]  = t_pico - t_proc;
        delta[PROF_BUCK_BLIT]  = t_blit - t_pico;   /* ~0 on skip: nothing renders */
        delta[PROF_BUCK_AUDIO] = t_audio - t_blit;
        delta[PROF_BUCK_TOTAL] = t_audio;           /* since the single clear */
        if (drawFrame) {
          uint32_t i = prof_drawn_count++;
          for (uint32_t b = 0; b < PROF_BUCK_COUNT; b++) {
            PROF_AT(prof_delta_drawn, b, i) = delta[b];
            prof_sum_drawn[b] += delta[b];
          }
        } else {
          uint32_t i = prof_skip_count++;
          for (uint32_t b = 0; b < PROF_BUCK_COUNT; b++) {
            PROF_AT(prof_delta_skip, b, i) = delta[b];
            prof_sum_skip[b] += delta[b];
          }
        }
      } else {
        /* Window full: one controlled SD dump (mutes audio, kicks wdog
         * internally), then never record or write again. */
        md32x_profile_dump();
        prof_dumped = true;
      }
    }
#endif
  }
}
