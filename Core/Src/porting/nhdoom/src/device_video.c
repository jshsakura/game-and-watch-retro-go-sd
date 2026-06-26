/* nhdoom DEVICE port: replaces src/graphics.c + src/display.c.
 *
 * The engine renders an 8bpp paletted frame into displayData.displayFrameBuffer
 * and publishes a (byte-swapped RGB565) palette via displayData.pPalette, then
 * calls startDisplayRefresh(buffer). On the Game & Watch we run the LCD in LUT8
 * mode: the LTDC's hardware CLUT does the colour lookup (programmed from the
 * engine palette), so the frame is just an index copy into the horizontally
 * centred middle of the active buffer (240 wide inside the 320 wide LCD).
 *
 * The ~116KB displayData double-buffer is NOT in the 256KB short-pointer window
 * (it is a render target, never Z_Malloc'd), so it is allocated from the
 * RAM_EMU overlay via ram_malloc, exactly like host_video.c kept it out of the
 * window with mmap.
 *
 * GPL-2.0 (see external/nh-doom, next-hack/nRF52840Doom lineage).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include "graphics.h"
#include "display.h"
#include "gw_lcd.h"
#include "gw_malloc.h"

#define NH_LCD_W       GW_LCD_WIDTH         /* 320 */
#define NH_X_OFFSET    ((GW_LCD_WIDTH  - NH_FB_W) / 2)   /* (320-240)/2 = 40 */
#define NH_Y_OFFSET    ((GW_LCD_HEIGHT - NH_FB_H) / 2)   /* (240-240)/2 = 0  */

displayData_t *nh_pDisplayData;
static uint16_t nh_palette_store[256];   /* engine palette target (small) */
static uint32_t nh_clut[256];            /* LTDC CLUT (0x00RRGGBB) */

void initGraphics(void)
{
    nh_pDisplayData = (displayData_t *)ram_malloc(sizeof(displayData_t));
    if (!nh_pDisplayData) {
        printf("[nhdoom] FATAL: displayData alloc failed (%u bytes)\n",
               (unsigned)sizeof(displayData_t));
        return;
    }
    memset(nh_pDisplayData, 0, sizeof(displayData_t));
    displayData.pPalette = nh_palette_store;
    displayData.displayMode = 8;
    displayData.workingBuffer = 0;
    displayData.dmaBusy = 0;

    lcd_setup_framebuffers(LCD_MODE_LUT8);
    lcd_clear_buffers();
}

/* one pPalette entry is byte-swapped RGB565 (see engine I_UploadNewPalette). */
static uint32_t rgb565_swapped_to_argb(uint16_t v)
{
    uint16_t p = (uint16_t)((v >> 8) | (v << 8));   /* undo byte swap */
    uint8_t r5 = (p >> 11) & 0x1F;
    uint8_t g6 = (p >> 5)  & 0x3F;
    uint8_t b5 =  p        & 0x1F;
    uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
    uint8_t g = (uint8_t)((g6 << 2) | (g6 >> 4));
    uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void startDisplayRefresh(uint8_t bufferNumber)
{
    if (!nh_pDisplayData) return;

    /* Program the LTDC CLUT from the engine palette. */
    const volatile uint16_t *pal = displayData.pPalette;
    if (pal) {
        for (int i = 0; i < 256; i++)
            nh_clut[i] = rgb565_swapped_to_argb((uint16_t)pal[i]);
        lcd_set_clut(nh_clut, 256);
    }

    /* Copy the 240x240 8bpp frame into the centred region of the active LCD
     * buffer (8bpp LUT8). Rows are not full-width, so copy row by row. */
    const uint8_t *src = displayData.displayFrameBuffer[bufferNumber];
    uint8_t *dst = (uint8_t *)lcd_get_active_buffer();
    if (dst) {
        dst += (size_t)NH_Y_OFFSET * NH_LCD_W + NH_X_OFFSET;
        for (int y = 0; y < NH_FB_H; y++) {
            memcpy(dst, src, NH_FB_W);
            src += NH_FB_W;
            dst += NH_LCD_W;
        }
        lcd_swap();
    }
}

/* boot-time text overlays just go to the trace log. */
void displayPrintf(int x, int y, const char *fmt, ...)
{ (void)x; (void)y; va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void displayPrintln(bool update, const char *fmt, ...)
{ (void)update; va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }
void setDisplayPen(int color, int background) { (void)color; (void)background; }
void clearScreen4bpp(void) {}

/* display.h LCD HAL — the LTDC drives the panel; these low-level SPI hooks are
 * unused on the Game & Watch parallel LCD. */
void UpdateDisplay(void) {}
void DisplayInit(void) {}
void BeginUpdateDisplay(void) {}
void EndUpdateDisplay(void) {}
void SelectDisplay(void) {}
void DisplayWriteData(uint8_t v) { (void)v; }
void initDisplaySpi(void) {}
void SetBacklight(bool on) { (void)on; }
