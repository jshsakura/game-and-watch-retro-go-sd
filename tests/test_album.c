/* Host test for the clock photo-album loader (rg_clock_album.c) — focus on the
 * BMP path, which parses untrusted external file bytes: header validation,
 * BGR->RGB565 conversion, bottom-up row flip, and rejection of wrong
 * size/format/compression.
 *
 * Build+run (see tests/run.sh):
 *   gcc -O2 -Wall -Wextra -std=gnu11 -Itests/album_stubs tests/test_album.c \
 *       Core/Src/retro-go/rg_clock_album.c -o /tmp/mtest/test_album && /tmp/mtest/test_album
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- pull in the unit under test with its host stubs (album_stubs) -------- */
#include "rg_clock_album.h"

/* private symbols the test drives are exposed by compiling the .c in the same
 * TU is not needed: we drive the public API (clock_album_open/current/advance)
 * against a real temp dir the scandir stub enumerates. */

/* scandir stub feeds these paths (set per test). */
extern void album_test_set_files(const char **paths, int n);

static int g_fail = 0;
#define CHECK(c, name) do { if (c) printf("OK  %s\n", name); \
    else { printf("FAIL %s\n", name); g_fail = 1; } } while (0)

#define W 320
#define H 240

static void write_565(const char *path, uint16_t fill)
{
    FILE *f = fopen(path, "wb");
    for (int i = 0; i < W * H; i++) fwrite(&fill, 2, 1, f);
    fclose(f);
}

/* 24-bit bottom-up BMP with a per-row solid colour derived from screen row y. */
static void write_bmp24(const char *path, bool top_down)
{
    FILE *f = fopen(path, "wb");
    uint32_t stride = W * 3;                 /* already 4-aligned */
    uint32_t datasz = stride * H, off = 54, filesz = off + datasz;
    int32_t  h = top_down ? -H : H;
    uint8_t hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    memcpy(hdr+2,  &filesz, 4);
    memcpy(hdr+10, &off, 4);
    uint32_t ihsz = 40; memcpy(hdr+14, &ihsz, 4);
    int32_t w = W; memcpy(hdr+18, &w, 4); memcpy(hdr+22, &h, 4);
    uint16_t planes = 1, bpp = 24;
    memcpy(hdr+26, &planes, 2); memcpy(hdr+28, &bpp, 2);
    fwrite(hdr, 1, 54, f);
    uint8_t *row = malloc(stride);
    for (int s = 0; s < H; s++) {
        int screen_y = top_down ? s : (H - 1 - s);
        /* encode screen_y into a colour: R=screen_y, G=0, B=255 */
        for (int x = 0; x < W; x++) { row[x*3+0]=255; row[x*3+1]=0; row[x*3+2]=(uint8_t)screen_y; }
        fwrite(row, 1, stride, f);
    }
    free(row);
    fclose(f);
}

static void write_bmp_bad(const char *path)   /* right magic, wrong size (100x100) */
{
    FILE *f = fopen(path, "wb");
    uint8_t hdr[54] = {0}; hdr[0]='B'; hdr[1]='M';
    uint32_t off=54; memcpy(hdr+10,&off,4);
    int32_t w=100,h=100; memcpy(hdr+18,&w,4); memcpy(hdr+22,&h,4);
    uint16_t bpp=24; memcpy(hdr+28,&bpp,2);
    fwrite(hdr,1,54,f); fclose(f);
}

static uint16_t px(int x, int y) { return clock_album_current()[y*W+x]; }

int main(void)
{
    if (system("mkdir -p /tmp/albtest") != 0) return 1;
    const char *p565 = "/tmp/albtest/a.565";
    const char *pbmp = "/tmp/albtest/b.bmp";
    const char *ptop = "/tmp/albtest/c.bmp";
    const char *pbad = "/tmp/albtest/d.bmp";

    /* 1) raw .565 loads and shows its fill colour */
    write_565(p565, 0xABCD);
    { const char *fs[] = { p565 }; album_test_set_files(fs, 1); }
    CHECK(clock_album_open(), "565 opens");
    CHECK(clock_album_ready(), "565 ready");
    CHECK(px(0,0) == 0xABCD && px(W-1,H-1) == 0xABCD, "565 pixels intact");
    clock_album_close();

    /* 2) bottom-up 24-bit BMP: row flip + BGR->565 correct.
     *    encoded colour was R=screen_y G=0 B=255 -> 565 = (y&0xF8)<<8 | 0 | (255>>3) */
    write_bmp24(pbmp, false);
    { const char *fs[] = { pbmp }; album_test_set_files(fs, 1); }
    CHECK(clock_album_open() && clock_album_ready(), "bmp bottom-up opens");
    {
        uint16_t top    = px(10, 0);     /* screen row 0  -> R=0   */
        uint16_t bottom = px(10, H - 1); /* screen row 239-> R=239 */
        uint16_t exp_top    = (uint16_t)(((0   & 0xF8) << 8) | (255 >> 3));
        uint16_t exp_bottom = (uint16_t)(((239 & 0xF8) << 8) | (255 >> 3));
        CHECK(top == exp_top, "bmp top row colour + flip");
        CHECK(bottom == exp_bottom, "bmp bottom row colour + flip");
    }
    clock_album_close();

    /* 3) top-down BMP (negative height) maps rows the other way, same result */
    write_bmp24(ptop, true);
    { const char *fs[] = { ptop }; album_test_set_files(fs, 1); }
    CHECK(clock_album_open() && clock_album_ready(), "bmp top-down opens");
    CHECK(px(10, H-1) == (uint16_t)(((239 & 0xF8) << 8) | (255 >> 3)), "bmp top-down row order");
    clock_album_close();

    /* 4) wrong-size BMP is rejected (not ready), no crash */
    write_bmp_bad(pbad);
    { const char *fs[] = { pbad }; album_test_set_files(fs, 1); }
    CHECK(!clock_album_open(), "bad-size bmp rejected");
    clock_album_close();

    /* 5) mixed dir: a 565 and a bmp both count, advance cycles */
    { const char *fs[] = { p565, pbmp }; album_test_set_files(fs, 2); }
    CHECK(clock_album_open() && clock_album_count() == 2, "mixed 565+bmp counted");
    CHECK(px(0,0) == 0xABCD, "mixed starts on 565");
    clock_album_advance();
    CHECK(px(10,0) == (uint16_t)(((0 & 0xF8) << 8) | (255 >> 3)), "advance to bmp");
    clock_album_close();

    printf(g_fail ? "\nALBUM TESTS FAILED\n" : "\nALL PASS\n");
    return g_fail;
}
