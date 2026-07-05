#ifndef STUB_GW_LCD_H
#define STUB_GW_LCD_H
#include <stdint.h>
#define GW_LCD_WIDTH 320
#define GW_LCD_HEIGHT 240
uint16_t *lcd_get_active_buffer(void);
void lcd_swap(void);
void lcd_sleep_while_swap_pending(void);
#endif
