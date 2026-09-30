/* Compiles the real Core/Src/gw_lcd_clut.c against a model of the LTDC CLUT
 * (256 slots, loaded from slot 0 like HAL_LTDC_ConfigCLUT) and checks what
 * the panel would show, not what the bookkeeping believes.
 *
 * The case that matters: a core that owns all 256 entries (Sega CD, via
 * lcd_set_clut_full) used to leave the menu colours at 0x40..0x43, which are
 * its own game colours, so every dialog drew in whatever the game had there;
 * the dim OR'ed 0x20 into pixels, onto more game colours; and a savestate
 * preview was saved with an all-zero palette (black thumbnail). */
#include <stdio.h>
#include <string.h>

#include "gw_lcd.h"

static uint32_t hw[256];
static int mode = LCD_MODE_LUT8;
int lcd_get_mode(void) { return mode; }
void lcd_clut_hw_load(const uint32_t *clut, uint16_t count)
{
  for (uint16_t i = 0; i < count && i < 256; i++) hw[i] = clut[i] & 0xffffff;
}
void lcd_clut_hw_set(uint8_t index, uint32_t rgb888) { hw[index] = rgb888 & 0xffffff; }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static uint16_t to565(uint32_t e)
{
  return (uint16_t)((((e >> 16) & 0xff) >> 3) << 11 | (((e >> 8) & 0xff) >> 2) << 5 | ((e & 0xff) >> 3));
}

static uint32_t game[256];
static const uint32_t menu[4] = { 0x10c010, 0xf0f0f0, 0x202020, 0xc01010 };

static void full_setup(void)
{
  /* Every game entry distinct and far from the menu colours. */
  for (int i = 0; i < 256; i++) game[i] = 0x400000 | (uint32_t)(i << 8) | 0x40;
  lcd_set_overlay_clut(menu, 4);
  lcd_set_clut_full(game, 256);
}

int main(void)
{
  /* 1. A menu colour must be what the panel shows at the index it packs to. */
  full_setup();
  for (int i = 0; i < 4; i++) {
    uint8_t idx = (uint8_t)lcd_pack_color(to565(menu[i]));
    CHECK(to565(hw[idx]) == to565(menu[i]),
          "menu colour %06lx packs to %02x, which shows %06lx", (unsigned long)menu[i], idx,
          (unsigned long)hw[idx]);
  }

#ifndef RED_OLD_API_ONLY
  /* ...and packing it must not have taken a slot the game draws with. */
  for (int i = 0; i < 4; i++) {
    uint8_t idx = (uint8_t)lcd_pack_color(to565(menu[i]));
    CHECK(idx >= LCD_FULL_GAME_ENTRIES, "menu colour packed into game slot %02x", idx);
  }
  /* The game's own slots are untouched by the menu. */
  CHECK(hw[0x40] == game[0x40] && hw[0x05] == game[0x05], "full palette overwritten in the game range");
  CHECK(lcd_clut_is_full(), "lcd_set_clut_full did not enter full-palette mode");

  /* 2. Dim: the palette darkens, pixels stay; menu slots stay exact. */
  lcd_clut_darken_full();
  for (int i = 0; i < LCD_FULL_GAME_ENTRIES; i++) {
    uint32_t e = game[i], k = 100 - LCD_DARKEN_PERCENT;
    uint32_t want = ((((e >> 16) & 0xff) * k / 100) << 16) | ((((e >> 8) & 0xff) * k / 100) << 8) | ((e & 0xff) * k / 100);
    if (hw[i] != want) { CHECK(0, "slot %02x not dimmed: %06lx", i, (unsigned long)hw[i]); break; }
  }
  for (int i = 0; i < 4; i++)
    CHECK(hw[LCD_FULL_OVERLAY_BASE + i] == menu[i], "dim touched menu slot %d", i);
  /* The next frame's palette push undoes the dim. */
  lcd_set_clut_full(game, 256);
  CHECK(hw[0x10] == game[0x10], "palette push did not restore the undimmed entry");

  /* 3. A pixel darken (volume box etc.) lands on the reserved black, not a game slot. */
  uint8_t d1 = lcd_lut8_darken_index(0x05);
  CHECK(d1 >= LCD_FULL_GAME_ENTRIES && to565(hw[d1]) == 0, "full-mode pixel darken went to %02x", d1);

  /* 4. Conversions use the game's palette: screenshots and previews. */
  uint8_t px[3] = { 0x00, 0x7f, (uint8_t)lcd_pack_color(to565(menu[1])) };
  uint16_t out[3];
  lcd_convert_lut8_to_rgb565(px, out, 3, NULL);
  CHECK(out[0] == to565(game[0]) && out[1] == to565(game[0x7f]) && out[2] == to565(menu[1]),
        "LUT8->RGB565 in full mode: %04x %04x %04x", out[0], out[1], out[2]);
  /* In-game preview of an RGB565 thumbnail: nearest game colour, exactly. */
  CHECK(lcd_pack_color(to565(game[0x33])) == 0x33 || to565(hw[lcd_pack_color(to565(game[0x33]))]) == to565(game[0x33]),
        "RGB565 -> LUT8 does not find the game's own colour");

  /* 5. A 32-colour cart core is exactly as before, and leaves full mode. */
  uint32_t cart[16];
  for (int i = 0; i < 16; i++) cart[i] = (uint32_t)(i * 0x0f0f0f);
  lcd_set_clut(cart, 16);
  CHECK(!lcd_clut_is_full(), "lcd_set_clut left full-palette mode on");
  CHECK(lcd_lut8_darken_index(0x03) == (0x03 | LCD_DARKEN_BIT), "cart darken is no longer the twin OR");
  CHECK(lcd_lut8_darken_index(0x03 | LCD_DARKEN_BIT) == 0, "cart darken-of-darken is no longer 0");
  CHECK(hw[16 + 5] != 0 && hw[LCD_OVERLAY_CLUT_BASE] == menu[0], "cart twins/overlay layout changed");
  CHECK(lcd_pack_color(to565(menu[2])) == LCD_OVERLAY_CLUT_BASE + 2, "cart-mode overlay packing changed");
#endif

  if (failures) return 1;
  printf("  OK   LUT8 CLUT: menu colours, dim, previews and pixel darken in full-palette mode\n");
  return 0;
}
