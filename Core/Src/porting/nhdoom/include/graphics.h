/* nhdoom host port: shadow of src/graphics.h.
 * Same displayData_t contract the engine relies on (8bpp double buffer +
 * RGB565 palette pointer), minus the nRF DMA line-buffer plumbing. */
#ifndef SRC_GRAPHICS_H_
#define SRC_GRAPHICS_H_
#include <stdbool.h>
#include <stdint.h>

#define NEW_DISPLAY_UPDATE_WAY 0
#define NH_FB_W 240
#define NH_FB_H 240

typedef struct {
    volatile uint16_t *pPalette;                 /* 256 entries, byte-swapped RGB565 */
    uint8_t  displayFrameBuffer[2][NH_FB_W * NH_FB_H];
    uint8_t  workingBuffer;
    uint8_t  displayMode;
    volatile uint8_t dmaBusy;
} displayData_t;

/* displayData is a ~116KB double framebuffer. It is never a Z_Malloc owner
 * (only a render target), so we keep it OUT of the 0x20000000 short-pointer
 * window by backing it with an mmap and exposing it through a pointer. The
 * engine's `displayData.field` accesses are unchanged via this macro. */
extern displayData_t *nh_pDisplayData;
#define displayData (*nh_pDisplayData)

void initGraphics(void);
void startDisplayRefresh(uint8_t bufferNumber);
void displayPrintf(int x, int y, const char *format, ...);
void displayPrintln(bool update, const char *format, ...);
void setDisplayPen(int color, int background);
void clearScreen4bpp(void);
#endif
