/* Full-palette LUT8 handling for the Sega CD core: PicoDrive's 8-bit
 * renderer owns CLUT slots 0x00..0xBF (normal, shadow, highlight) and leaves
 * the top to the frontend's OSD. The shared menu code draws through
 * lcd_pack_color / lcd_pen_darken / odroid_overlay_darken_all and the
 * savestate thumbnail goes through lcd_convert_lut8_to_rgb565; with a full
 * palette they need the answers below (gw_lcd.h, lcd_clut_full_ops_t).
 * Lives in this overlay, not in internal flash, which has no room for it. */
#include <stdint.h>
#include <stdbool.h>

#include "gw_lcd.h"

static const uint32_t *full_clut;
static uint16_t full_count;

static uint16_t game_slots(void)
{
  return full_count < LCD_FULL_GAME_ENTRIES ? full_count : LCD_FULL_GAME_ENTRIES;
}

static void full_reserved(void)
{
  uint16_t n;
  const uint32_t *theme = lcd_clut_theme(&n);
  for (uint16_t i = 0; i < n; i++)
    lcd_clut_hw_set((uint8_t)(LCD_FULL_OVERLAY_BASE + i), theme[i]);
  lcd_clut_hw_set(LCD_FULL_BLACK_INDEX, 0);
}

static uint32_t full_entry(uint8_t idx)
{
  if (idx < LCD_FULL_GAME_ENTRIES) return idx < full_count ? full_clut[idx] : 0;
  uint16_t n;
  const uint32_t *theme = lcd_clut_theme(&n);
  if (idx >= LCD_FULL_OVERLAY_BASE && idx < LCD_FULL_OVERLAY_BASE + n)
    return theme[idx - LCD_FULL_OVERLAY_BASE];
  return 0;
}

static void nearest(const uint32_t *tab, uint16_t n, int base,
                    int r, int g, int b, int *best_dist, int *best_idx)
{
  for (uint16_t i = 0; i < n && *best_dist; i++) {
    uint32_t e = tab[i];
    int dr = r - (int)((e >> 16) & 0xFF);
    int dg = g - (int)((e >>  8) & 0xFF);
    int db = b - (int)((e      ) & 0xFF);
    int d  = dr*dr + dg*dg + db*db;
    if (d < *best_dist) { *best_dist = d; *best_idx = base + (int)i; }
  }
}

/* Menu colours first (exact match wins), then the game's own slots, so an
 * RGB565 thumbnail shown in-game maps back onto the palette. */
static uint16_t full_pack(uint16_t rgb565)
{
  int r = ((rgb565 >> 11) & 0x1F) * 255 / 31;
  int g = ((rgb565 >>  5) & 0x3F) * 255 / 63;
  int b = ((rgb565      ) & 0x1F) * 255 / 31;
  int best_dist = 0x7FFFFFFF, best_idx = LCD_FULL_BLACK_INDEX;
  uint16_t n;
  const uint32_t *theme = lcd_clut_theme(&n);
  nearest(theme, n, LCD_FULL_OVERLAY_BASE, r, g, b, &best_dist, &best_idx);
  nearest(full_clut, game_slots(), 0, r, g, b, &best_dist, &best_idx);
  return (uint16_t)best_idx;
}

static void full_darken(void)
{
  const uint32_t keep = 100 - LCD_DARKEN_PERCENT;
  for (uint16_t i = 0; i < game_slots(); i++) {
    uint32_t e = full_clut[i];
    lcd_clut_hw_set((uint8_t)i, ((((e >> 16) & 0xFF) * keep / 100) << 16) |
                                ((((e >>  8) & 0xFF) * keep / 100) <<  8) |
                                 (((e      ) & 0xFF) * keep / 100));
  }
}

static const lcd_clut_full_ops_t full_ops = {
  full_pack, full_entry, full_darken, full_reserved,
};

/* `clut` must outlive the core's use of the LCD (the caller's static). */
void lcd_set_clut_full(const uint32_t *clut, uint16_t count)
{
  if (lcd_get_mode() != LCD_MODE_LUT8 || clut == NULL || count == 0) return;
  if (count > 256) count = 256;
  full_clut = clut;
  full_count = count;
  lcd_clut_set_full_ops(&full_ops);
  lcd_clut_hw_load(clut, count);
  full_reserved();
}
