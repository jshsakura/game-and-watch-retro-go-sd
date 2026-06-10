#pragma once
#include <stdint.h>
#include <stdbool.h>
#define GW_LCD_WIDTH 320
#define GW_LCD_HEIGHT 240
void *lcd_get_active_buffer(void);
void *lcd_clear_active_buffer(void);
void lcd_swap(void);
uint8_t lcd_backlight_get(void);
void lcd_backlight_set(uint8_t b);
void lcd_backlight_on(void);
void lcd_backlight_off(void);
