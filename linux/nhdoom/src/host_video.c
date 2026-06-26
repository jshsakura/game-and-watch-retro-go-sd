/* nhdoom host port: replaces src/graphics.c + src/display.c.
 * The engine renders an 8bpp frame into displayData.displayFrameBuffer and
 * publishes an RGB565 palette via displayData.pPalette, then calls
 * startDisplayRefresh().  Here that becomes a PPM dump of the just-rendered
 * buffer, and after a configured number of frames the process exits. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/mman.h>
#include "graphics.h"
#include "display.h"

/* mmap-backed framebuffer, outside the 0x20000000 short-pointer window. */
displayData_t *nh_pDisplayData;

static uint16_t nh_palette_store[256];   /* small, fine inside the window */

static int   g_frame = 0;
static int   g_frame_limit = 240;
static int   g_first_dump = 0;
static char  g_outdir[1024] = ".";

void initGraphics(void)
{
    nh_pDisplayData = mmap(NULL, sizeof(displayData_t), PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nh_pDisplayData == MAP_FAILED) { perror("mmap displayData"); exit(2); }

    displayData.pPalette = nh_palette_store;
    displayData.displayMode = 8;
    displayData.workingBuffer = 0;
    displayData.dmaBusy = 0;

    const char *d = getenv("NHDOOM_PPM_DIR");
    if (d) { snprintf(g_outdir, sizeof g_outdir, "%s", d); }
    const char *lim = getenv("NHDOOM_FRAMES");
    if (lim) g_frame_limit = atoi(lim);
    const char *fd = getenv("NHDOOM_FIRST_DUMP");
    if (fd) g_first_dump = atoi(fd);
}

/* one entry of pPalette is byte-swapped RGB565 (see I_UploadNewPalette). */
static void rgb565_swapped_to_rgb(uint16_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint16_t p = (uint16_t)((v >> 8) | (v << 8));   /* undo byte swap */
    uint8_t r5 = (p >> 11) & 0x1F;
    uint8_t g6 = (p >> 5)  & 0x3F;
    uint8_t b5 =  p        & 0x1F;
    *r = (uint8_t)((r5 << 3) | (r5 >> 2));
    *g = (uint8_t)((g6 << 2) | (g6 >> 4));
    *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

static void dump_ppm(int idx, const uint8_t *fb)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/frame_%04d.ppm", g_outdir, idx);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", NH_FB_W, NH_FB_H);
    const volatile uint16_t *pal = displayData.pPalette;
    for (int i = 0; i < NH_FB_W * NH_FB_H; i++) {
        uint8_t r = 0, g = 0, b = 0;
        if (pal) rgb565_swapped_to_rgb((uint16_t)pal[fb[i]], &r, &g, &b);
        else { r = g = b = fb[i]; }
        fputc(r, f); fputc(g, f); fputc(b, f);
    }
    fclose(f);
}

void startDisplayRefresh(uint8_t bufferNumber)
{
    const uint8_t *fb = displayData.displayFrameBuffer[bufferNumber];
    if (g_frame >= g_first_dump)
        dump_ppm(g_frame, fb);
    g_frame++;
    if (g_frame >= g_frame_limit) {
        fflush(stdout);
        fprintf(stderr, "[nhdoom] frame limit %d reached, exiting\n", g_frame_limit);
        exit(0);
    }
}

/* text overlays during boot just go to stdout. */
void displayPrintf(int x, int y, const char *fmt, ...)
{ (void)x; (void)y; va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void displayPrintln(bool update, const char *fmt, ...)
{ (void)update; va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }
void setDisplayPen(int color, int background) { (void)color; (void)background; }
void clearScreen4bpp(void) {}

/* display.h LCD HAL — all no-ops on host. */
void UpdateDisplay(void) {}
void DisplayInit(void) {}
void BeginUpdateDisplay(void) {}
void EndUpdateDisplay(void) {}
void SelectDisplay(void) {}
void DisplayWriteData(uint8_t v) { (void)v; }
void initDisplaySpi(void) {}
void SetBacklight(bool on) { (void)on; }
