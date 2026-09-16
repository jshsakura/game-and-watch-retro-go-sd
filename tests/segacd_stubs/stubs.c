/* Stub bodies + the LCD line recorder for the Sega CD RAM probe host test.
 * See tests/segacd_stubs/main.h for the layout rationale. */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "stm32h7xx_hal.h"
#include "gw_lcd.h"
#include "odroid_overlay.h"
#include "odroid_display.h"

#define SCD_MAX_LINES 32
char scd_lines[SCD_MAX_LINES][96];
int scd_nlines;
int scd_overflow;
jmp_buf scd_halt;   /* the probe's forever loop jumps back through HAL_Delay */

void wdog_refresh(void) {}
void lcd_backlight_off(void) {}
void lcd_sync(void) {}
void odroid_display_set_backlight(odroid_display_backlight_t level)
{
    (void)level;
}

int odroid_overlay_draw_text(uint16_t x, uint16_t y, uint16_t width,
                             const char *text, uint16_t color,
                             uint16_t color_bg)
{
    (void)x; (void)width; (void)color; (void)color_bg;
    if (scd_nlines < SCD_MAX_LINES) {
        snprintf(scd_lines[scd_nlines], sizeof(scd_lines[0]), "%s", text);
#ifdef SCD_RED_TEST
        /* RED mode: corrupt the FIRST region line so the passing sweep has a
         * fault the checker must catch. Proves the assertions below bite. */
        if (scd_nlines >= 2 && strstr(scd_lines[scd_nlines], " ok")) {
            strcpy(scd_lines[scd_nlines] + strlen(scd_lines[scd_nlines]) - 2,
                   "RW FAIL");
        }
#endif
        scd_nlines++;
    } else {
        scd_overflow = 1;
    }
    return y + 12;   /* firmware line advance */
}

void HAL_Delay(uint32_t ms)
{
    (void)ms;
    longjmp(scd_halt, 1);   /* the probe has halted: hand control back */
}
