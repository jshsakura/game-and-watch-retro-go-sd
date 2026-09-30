/* LTDC colour lookup table bookkeeping for LUT8 mode, split out of gw_lcd.c
 * so tests/test_lcd_clut.c can compile it on the host. The hardware is
 * reached only through lcd_clut_hw_load()/lcd_clut_hw_set() (gw_lcd.c). */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "gw_lcd.h"

/* Active CLUT staged for HAL_LTDC_ConfigCLUT and reused by lcd_pack_color()
 * for nearest-match RGB→index lookups.
 *
 * Layout (sized to fit DTCM BSS budget — bumped only by ~16 bytes vs the
 * cart-only 64-entry baseline):
 *   [0..32)         cart palette entries (count ≤ 32)
 *   [32..64)        cart palette darkened twins (LCD_DARKEN_BIT path)
 *   [64..64+OMAX)   Retro-Go overlay theme entries (LCD_OVERLAY_CLUT_BASE)
 *
 * No darkened-twin slots for the overlay — see gw_lcd.h for rationale.
 * Setting LCD_DARKEN_BIT (0x20) on a cart pixel byte switches it to the
 * darker twin; the LTDC's CLUT does the lookup at scanout. */
#define LCD_CLUT_CACHE_MAX     32
#define LCD_EXTENDED_CLUT_MAX  (LCD_CLUT_CACHE_MAX * 2 + LCD_OVERLAY_CLUT_MAX)
static uint32_t active_clut[LCD_EXTENDED_CLUT_MAX];
static uint16_t active_clut_count   = 0;   /* cart entries in [0..count); twins at [count..2*count) */
static uint16_t overlay_clut_count  = 0;   /* overlay entries in [BASE..BASE+count) */

/* A core that owns all 256 slots (Sega CD) registers its own handling
 * (lcd_clut_full_ops_t, gw_lcd.h); it lives in that core's overlay. */
static const lcd_clut_full_ops_t *full_ops = NULL;

void lcd_clut_set_full_ops(const lcd_clut_full_ops_t *ops) { full_ops = ops; }
bool lcd_clut_is_full(void) { return full_ops != NULL; }

void lcd_clut_darken_full(void)
{
  if (full_ops != NULL && lcd_get_mode() == LCD_MODE_LUT8) full_ops->darken();
}

const uint32_t *lcd_clut_theme(uint16_t *count)
{
  *count = overlay_clut_count;
  return &active_clut[LCD_OVERLAY_CLUT_BASE];
}

uint8_t lcd_lut8_darken_index(uint8_t index)
{
  if (full_ops != NULL) return LCD_FULL_BLACK_INDEX;
  return (index & LCD_DARKEN_BIT) ? 0 : (uint8_t)(index | LCD_DARKEN_BIT);
}

/* Push the entire populated range to the LTDC. The HAL writes index =
 * counter, so we always send slot 0 onward. Determine how far we need
 * to go from whichever range is populated. */
static void clut_push(void)
{
  int total = (int)(2u * active_clut_count);
  if (overlay_clut_count > 0) {
    int overlay_end = LCD_OVERLAY_CLUT_BASE + overlay_clut_count;
    if (overlay_end > total) total = overlay_end;
  }
  if (total <= 0) return;
  lcd_clut_hw_load(active_clut,(uint16_t)total);
}

/* Compute and store a 40%-darkened twin of `e` (RGB888) at slot `idx`. */
static void clut_store_dark_twin(int idx, uint32_t e)
{
  int keep = 100 - LCD_DARKEN_PERCENT;
  uint32_t r = (((e >> 16) & 0xFF) * keep) / 100;
  uint32_t g = (((e >>  8) & 0xFF) * keep) / 100;
  uint32_t b = (((e      ) & 0xFF) * keep) / 100;
  active_clut[idx] = (r << 16) | (g << 8) | b;
}

static uint16_t rgb888_entry_to_rgb565(uint32_t e)
{
  uint8_t r = (uint8_t)((e >> 16) & 0xFF);
  uint8_t g = (uint8_t)((e >>  8) & 0xFF);
  uint8_t b = (uint8_t)((e      ) & 0xFF);
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Darken an RGB565 color by LCD_DARKEN_PERCENT — mirrors clut_store_dark_twin
 * so embedded cart CLUTs can reconstruct the [32..64) darkened-twin range. */
static uint16_t darken_rgb565(uint16_t c)
{
  const int keep = 100 - LCD_DARKEN_PERCENT;
  int r = (c >> 11) & 0x1F;
  int g = (c >>  5) & 0x3F;
  int b = (c      ) & 0x1F;
  r = (r * keep) / 100;
  g = (g * keep) / 100;
  b = (b * keep) / 100;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

void lcd_get_clut_rgb565(uint16_t *out)
{
  if (out == NULL) return;
  uint16_t n = active_clut_count;
  if (n > LCD_SCREENSHOT_CLUT_ENTRIES) n = LCD_SCREENSHOT_CLUT_ENTRIES;
  for (uint16_t i = 0; i < n; i++) {
    out[i] = rgb888_entry_to_rgb565(active_clut[i]);
  }
  for (uint16_t i = n; i < LCD_SCREENSHOT_CLUT_ENTRIES; i++) out[i] = 0;
}

void lcd_convert_lut8_to_rgb565(const uint8_t *src, uint16_t *dst, size_t count,
                                const uint16_t *clut)
{
  if (src == NULL || dst == NULL) return;

  for (size_t i = 0; i < count; i++) {
    uint8_t idx = src[i];
    if (clut != NULL) {
      if (idx < LCD_SCREENSHOT_CLUT_ENTRIES) {
        dst[i] = clut[idx];
      } else if (idx < 2 * LCD_SCREENSHOT_CLUT_ENTRIES) {
        dst[i] = darken_rgb565(clut[idx - LCD_SCREENSHOT_CLUT_ENTRIES]);
      } else {
        dst[i] = 0;
      }
    } else if (full_ops != NULL) {
      dst[i] = rgb888_entry_to_rgb565(full_ops->entry(idx));
    } else if (idx < LCD_EXTENDED_CLUT_MAX) {
      dst[i] = rgb888_entry_to_rgb565(active_clut[idx]);
    } else {
      dst[i] = 0;
    }
  }
}

void lcd_set_clut(const uint32_t *clut, uint16_t count)
{
  if (lcd_get_mode() != LCD_MODE_LUT8 || clut == NULL || count == 0) return;
  if (count > LCD_CLUT_CACHE_MAX) count = LCD_CLUT_CACHE_MAX;
  full_ops = NULL;

  /* Cart palette → [0..count); darkened twins → [count..2*count). */
  for (uint16_t i = 0; i < count; i++) {
    active_clut[i] = clut[i];
    clut_store_dark_twin(count + i, clut[i]);
  }
  active_clut_count = count;
  clut_push();
}

void lcd_set_overlay_clut(const uint32_t *colors, uint16_t count)
{
  if (colors == NULL) return;
  if (count > LCD_OVERLAY_CLUT_MAX) count = LCD_OVERLAY_CLUT_MAX;

  /* Always store the values (so they survive a later mode switch into
   * LUT8). Push to hardware only if we're already in LUT8 — otherwise
   * the next lcd_set_clut() call from the cart will pick them up via
   * clut_push() since active_clut[] is already populated. */
  for (uint16_t i = 0; i < count; i++) {
    active_clut[LCD_OVERLAY_CLUT_BASE + i] = colors[i];
  }
  overlay_clut_count = count;
  if (lcd_get_mode() != LCD_MODE_LUT8) return;
  if (full_ops != NULL) full_ops->reserved();
  else                  clut_push();
}

uint16_t lcd_pack_color(uint16_t rgb565)
{
  if (lcd_get_mode() != LCD_MODE_LUT8) return rgb565;
  if (full_ops != NULL) return full_ops->pack(rgb565);
  if (active_clut_count == 0 && overlay_clut_count == 0) return 0;

  /* Decode RGB565 → RGB888 components for distance comparison. */
  int r = ((rgb565 >> 11) & 0x1F) * 255 / 31;
  int g = ((rgb565 >>  5) & 0x3F) * 255 / 63;
  int b = ((rgb565      ) & 0x1F) * 255 / 31;

  int best_dist = 0x7FFFFFFF;
  int best_idx  = 0;

  /* Scan overlay first — exact-match menu colors win (distance 0). */
  for (uint16_t i = 0; i < overlay_clut_count; i++) {
    uint32_t e = active_clut[LCD_OVERLAY_CLUT_BASE + i];
    int er = (int)((e >> 16) & 0xFF);
    int eg = (int)((e >>  8) & 0xFF);
    int eb = (int)((e      ) & 0xFF);
    int dr = r - er, dg = g - eg, db = b - eb;
    int d  = dr*dr + dg*dg + db*db;
    if (d < best_dist) { best_dist = d; best_idx = LCD_OVERLAY_CLUT_BASE + (int)i; }
  }
  /* Then cart palette. The darkened twins are NOT searched — they're for
   * the +LCD_DARKEN_BIT OR path, not direct color matching. */
  for (uint16_t i = 0; i < active_clut_count; i++) {
    uint32_t e = active_clut[i];
    int er = (int)((e >> 16) & 0xFF);
    int eg = (int)((e >>  8) & 0xFF);
    int eb = (int)((e      ) & 0xFF);
    int dr = r - er, dg = g - eg, db = b - eb;
    int d  = dr*dr + dg*dg + db*db;
    if (d < best_dist) { best_dist = d; best_idx = (int)i; }
  }
  return (uint16_t)(best_idx & 0xFF);
}

