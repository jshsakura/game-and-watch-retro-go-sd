/* Host-test stub. Copy of the shape tests/clock_stubs/gw_lcd.h already uses —
 * kept as a separate copy per test-scope (tests/video_stubs/), not shared,
 * so a change here can't silently affect the Clock tests. */
#ifndef STUB_GW_LCD_H
#define STUB_GW_LCD_H
#include <stdint.h>
#define GW_LCD_WIDTH 320
#define GW_LCD_HEIGHT 240
uint16_t *lcd_get_active_buffer(void);
void lcd_swap(void);
void lcd_sleep_while_swap_pending(void);
void lcd_backlight_set(uint8_t brightness);
#endif
