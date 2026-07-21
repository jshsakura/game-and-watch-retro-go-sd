#ifndef STUB_GW_LCD_H
#define STUB_GW_LCD_H
#include <stdint.h>

#define GW_LCD_WIDTH  320
#define GW_LCD_HEIGHT 240
typedef uint16_t pixel_t;

/* draw_img()/draw_rectangle()/etc. in common.c are not under test here (no
 * pacing/allocator logic in them) -- lcd_pen_t is opaque, its accessors are
 * no-ops the test .c provides so those functions still link. */
typedef struct { int unused; } lcd_pen_t;
lcd_pen_t lcd_pen(uint16_t color);
void lcd_pen_set(const lcd_pen_t *p, int off);
void lcd_pen_run(const lcd_pen_t *p, int off, int count);
void lcd_pen_darken(const lcd_pen_t *p, int off);

pixel_t *lcd_get_active_buffer(void);
pixel_t *lcd_get_inactive_buffer(void);
void lcd_sleep_while_swap_pending(void);
void lcd_clear_active_buffer(void);
void lcd_sync(void);

#endif
