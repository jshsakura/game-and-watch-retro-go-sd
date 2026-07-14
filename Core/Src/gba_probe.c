/* What can the external flash actually feed a CPU?
 *
 * A GBA ROM is 4-32 MB. It cannot live in RAM, so a GBA core would have to read
 * it in place from memory-mapped OSPI flash at 0x90000000 — the same window
 * Super Metroid's 3 MB ROM already sits in. Before anyone writes a line of
 * emulator, this measures what that window costs, on the real chip, at both
 * clock settings.
 *
 * Held at boot: GAME + TIME. Nothing else in the firmware changes.
 *
 * The numbers to want:
 *   - sequential MB/s   : a DMA of tiles/samples, and a dynarec reading a block
 *   - cache-line fill   : what a cold 32-byte line costs
 *   - random 4-byte     : the worst case — a game reading a table at random
 *   - hot (cached)      : the ceiling, to show the D-cache is doing its job
 *   - internal SRAM     : the same loops with no flash, to subtract loop overhead
 */

#include "gba_probe.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "gw_lcd.h"
#include "gw_flash.h"
#include "gw_buttons.h"
#include "stm32h7xx_hal.h"

#include "odroid_colors.h"
#include "odroid_system.h"
#include "odroid_display.h"
#include "odroid_overlay.h"
#include "gittag.h"

#define EXTFLASH_BASE   ((const volatile uint32_t *)0x90000000u)

/* Stay above the firmware's own extflash image; any address is safe to READ,
 * this is only about being honest about what we are reading. */
#define PROBE_OFFSET_B  (1u * 1024 * 1024)

#define SEQ_BYTES       (256u * 1024)   /* > 32 KB D-cache, so it stays cold */
#define LINE_BYTES      (32u)           /* Cortex-M7 cache line */
#define RANDOM_READS    (4096u)
#define HOT_BYTES       (16u * 1024)    /* < D-cache, so it stays hot */
#define HOT_PASSES      (16u)

/* The reads must not be optimised away. */
static volatile uint32_t g_sink;

static uint32_t dwt_now(void) { return *(volatile uint32_t *)0xE0001004u; }

static void dwt_start(void) {
  *(volatile uint32_t *)0xE000EDFC |= 0x01000000u;  /* DEMCR: trace enable */
  *(volatile uint32_t *)0xE0001FB0  = 0xC5ACCE55u;  /* LAR: unlock */
  *(volatile uint32_t *)0xE0001004  = 0u;           /* CYCCNT = 0 */
  *(volatile uint32_t *)0xE0001000 |= 1u;           /* CTRL: enable */
}

/* Cold means cold: clean first so nothing dirty is lost, then invalidate. */
static void cache_drop(void) {
  SCB_CleanInvalidateDCache();
  __DSB();
  __ISB();
}

typedef struct {
  uint32_t seq_cycles;      /* SEQ_BYTES, sequential, cold */
  uint32_t line_cycles;     /* SEQ_BYTES / LINE_BYTES cold line fills */
  uint32_t rnd_cycles;      /* RANDOM_READS scattered 4-byte reads, cold */
  uint32_t hot_cycles;      /* HOT_BYTES * HOT_PASSES, cached */
  uint32_t memcpy_cycles;   /* 64 KB flash -> RAM */
  uint32_t sram_seq_cycles; /* the same sequential loop, from internal SRAM */
} probe_result_t;

/* A 64 KB landing zone for the memcpy test, and the SRAM baseline's source.
 * Lives in the emulator RAM pool — nothing else is running. */
extern uint32_t __RAM_EMU_START__;

static void run_pass(probe_result_t *r, uint32_t flash_words, uint32_t range_words) {
  const volatile uint32_t *flash = EXTFLASH_BASE + (PROBE_OFFSET_B / 4);
  uint32_t *sram = (uint32_t *)&__RAM_EMU_START__;
  uint32_t t0;

  /* 1. sequential, cold */
  cache_drop();
  t0 = dwt_now();
  for (uint32_t i = 0; i < flash_words; i++)
    g_sink += flash[i];
  r->seq_cycles = dwt_now() - t0;

  /* 2. one word per cache line, cold — isolates the line-fill cost */
  cache_drop();
  t0 = dwt_now();
  for (uint32_t i = 0; i < flash_words; i += LINE_BYTES / 4)
    g_sink += flash[i];
  r->line_cycles = dwt_now() - t0;

  /* 3. random 4-byte reads over the whole chip — the D-cache cannot help */
  cache_drop();
  uint32_t lcg = 12345u;
  t0 = dwt_now();
  for (uint32_t i = 0; i < RANDOM_READS; i++) {
    lcg = lcg * 1103515245u + 12345u;
    g_sink += EXTFLASH_BASE[(lcg >> 4) % range_words];
  }
  r->rnd_cycles = dwt_now() - t0;

  /* 4. hot: re-read a block that fits in the 32 KB D-cache */
  cache_drop();
  for (uint32_t i = 0; i < HOT_BYTES / 4; i++)   /* prime it */
    g_sink += flash[i];
  t0 = dwt_now();
  for (uint32_t p = 0; p < HOT_PASSES; p++)
    for (uint32_t i = 0; i < HOT_BYTES / 4; i++)
      g_sink += flash[i];
  r->hot_cycles = dwt_now() - t0;

  /* 5. memcpy 64 KB flash -> RAM: what a DMA of tiles costs */
  cache_drop();
  t0 = dwt_now();
  memcpy(sram, (const void *)flash, 64u * 1024);
  r->memcpy_cycles = dwt_now() - t0;

  /* 6. the same sequential loop from internal SRAM — subtract this to see the
   *    flash cost alone rather than the loop's own bookkeeping. */
  cache_drop();
  t0 = dwt_now();
  for (uint32_t i = 0; i < flash_words; i++)
    g_sink += sram[i & ((64u * 1024 / 4) - 1)];
  r->sram_seq_cycles = dwt_now() - t0;
}

/* cycles -> MB/s, given bytes moved and the core clock. */
static uint32_t mb_per_s(uint32_t bytes, uint32_t cycles, uint32_t hz) {
  if (cycles == 0) return 0;
  /* bytes/cycle * hz / 1e6, in integer arithmetic that cannot overflow */
  return (uint32_t)(((uint64_t)bytes * hz) / ((uint64_t)cycles * 1000000u));
}

/* cycles -> nanoseconds */
static uint32_t ns_per(uint32_t cycles, uint32_t count, uint32_t hz) {
  if (count == 0 || hz == 0) return 0;
  return (uint32_t)(((uint64_t)cycles * 1000000000u) / ((uint64_t)count * hz));
}

static int report(int y, const char *text) {
  return odroid_overlay_draw_text(0, y, GW_LCD_WIDTH, (char *)text, C_WHITE, C_BLUE);
}

static void report_pass(int *y, const char *title, const probe_result_t *r,
                        uint32_t hz, uint32_t flash_words) {
  char line[64];
  const uint32_t seq_bytes = flash_words * 4;
  const uint32_t lines     = flash_words / (LINE_BYTES / 4);

  snprintf(line, sizeof(line), "%s  core=%luMHz", title, (unsigned long)(hz / 1000000u));
  *y += odroid_overlay_draw_text(0, *y, GW_LCD_WIDTH, line, C_GREEN, C_BLUE);

  snprintf(line, sizeof(line), " seq cold   %4lu MB/s",
           (unsigned long)mb_per_s(seq_bytes, r->seq_cycles, hz));
  *y += report(*y, line);

  snprintf(line, sizeof(line), " line fill  %4lu ns  (%lu cyc)",
           (unsigned long)ns_per(r->line_cycles, lines, hz),
           (unsigned long)(r->line_cycles / (lines ? lines : 1)));
  *y += report(*y, line);

  snprintf(line, sizeof(line), " random 4B  %4lu ns  (%lu cyc)",
           (unsigned long)ns_per(r->rnd_cycles, RANDOM_READS, hz),
           (unsigned long)(r->rnd_cycles / RANDOM_READS));
  *y += report(*y, line);

  snprintf(line, sizeof(line), " hot cached %4lu MB/s",
           (unsigned long)mb_per_s(HOT_BYTES * HOT_PASSES, r->hot_cycles, hz));
  *y += report(*y, line);

  snprintf(line, sizeof(line), " memcpy 64K %4lu MB/s",
           (unsigned long)mb_per_s(64u * 1024, r->memcpy_cycles, hz));
  *y += report(*y, line);

  snprintf(line, sizeof(line), " SRAM seq   %4lu MB/s  (loop baseline)",
           (unsigned long)mb_per_s(seq_bytes, r->sram_seq_cycles, hz));
  *y += report(*y, line);
}

void gba_probe_run_if_requested(uint32_t boot_buttons) {
  if ((boot_buttons & (B_GAME | B_TIME)) != (B_GAME | B_TIME))
    return;

  const uint32_t flash_bytes = OSPI_GetFlashSize();
  const uint32_t flash_words = SEQ_BYTES / 4;
  /* Random reads must range over more than the D-cache, but stay on the chip. */
  const uint32_t range_words = (flash_bytes > PROBE_OFFSET_B)
                                 ? (flash_bytes / 4)
                                 : (SEQ_BYTES / 4);

  dwt_start();
  lcd_sync();
  lcd_reset_active_buffer();
  odroid_display_set_backlight(ODROID_BACKLIGHT_LEVEL6);

  int y = 0;
  char line[64];
  snprintf(line, sizeof(line), "OSPI XIP PROBE  flash=%luMB  %s",
           (unsigned long)(flash_bytes / (1024u * 1024u)), GIT_TAG);
  y += odroid_overlay_draw_text(0, y, GW_LCD_WIDTH, line, C_RED, C_BLUE);
  y += 4;

  probe_result_t normal, boost;

  SystemClock_Config(0);                       /* 280 MHz core, OSPI 64 MHz */
  run_pass(&normal, flash_words, range_words);
  report_pass(&y, "NORMAL (OSPI 64MHz)", &normal, HAL_RCC_GetSysClockFreq(), flash_words);
  y += 4;

  SystemClock_Config(2);                       /* 340 MHz core, OSPI 97 MHz */
  run_pass(&boost, flash_words, range_words);
  report_pass(&y, "BOOST2 (OSPI 97MHz)", &boost, HAL_RCC_GetSysClockFreq(), flash_words);

  y += 4;
  report(y, "Photograph this. Power-cycle to boot normally.");

  lcd_swap();
  while (1) {
    wdog_refresh();
    lcd_sync();
    HAL_Delay(50);
  }
}
