/* Compiles the real Core/Src/porting/md32x/md32x_fullscreen.c, never a copy.
 *
 * What this has to catch is the failure mode an in-place expansion has and a
 * scaler into a second buffer does not: reading a row after something else has
 * already written over it. That corruption is silent -- the picture is still a
 * picture, just smeared -- so the test tags every source row with its own
 * number and then checks that each output row carries the number the
 * nearest-neighbour map says it should.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "md32x_fullscreen.h"

#define W MD32X_FS_WIDTH
#define H MD32X_FS_HEIGHT

static int failures = 0;

static void ok(int cond, const char *what)
{
    printf("%s  %s\n", cond ? "OK  " : "FAIL", what);
    if (!cond) failures++;
}

/* Paint every row of the content rect with a value derived from its row
 * number, and the border rows with a value that must not survive. */
static void paint(uint16_t *fb, int top, int lines)
{
    for (int y = 0; y < H; y++) {
        uint16_t v = (y >= top && y < top + lines) ? (uint16_t)(0x1000 + y)
                                                   : (uint16_t)0xDEAD;
        for (int x = 0; x < W; x++) fb[(size_t)y * W + x] = v;
    }
}

static int check_map(uint16_t *fb, int top, int lines, const char *what)
{
    int bad = -1;
    for (int y = 0; y < H; y++) {
        int src = top + (int)(((long)y * lines) / H);
        uint16_t want = (uint16_t)(0x1000 + src);
        for (int x = 0; x < W; x++) {
            if (fb[(size_t)y * W + x] != want) { bad = y; break; }
        }
        if (bad >= 0) break;
    }
    if (bad >= 0)
        printf("     row %d holds %04x, wanted %04x\n", bad,
               fb[(size_t)bad * W], (uint16_t)(0x1000 + top + (int)(((long)bad * lines) / H)));
    ok(bad < 0, what);
    return bad < 0;
}

int main(void)
{
    static uint16_t fb[(size_t)W * H];

    printf("=== md32x fullscreen: V28 224 -> 240 in place ===\n");
    paint(fb, 8, 224);
    md32x_fullscreen_expand(fb, 8, 224);
    check_map(fb, 8, 224, "every output row is the row the map says, none smeared");

    /* The bars are the point of the exercise. */
    {
        int bars_clean = 1;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < W; x++)
                if (fb[(size_t)y * W + x] == 0xDEAD) bars_clean = 0;
        for (int y = H - 8; y < H; y++)
            for (int x = 0; x < W; x++)
                if (fb[(size_t)y * W + x] == 0xDEAD) bars_clean = 0;
        ok(bars_clean, "no border pixel survives: the 8-row bars are gone");
    }

    /* Exactly 16 rows must be duplicates, one in every 14. */
    {
        int dups = 0;
        for (int y = 1; y < H; y++)
            if (fb[(size_t)y * W] == fb[(size_t)(y - 1) * W]) dups++;
        ok(dups == H - 224, "exactly 16 duplicated rows (240 - 224)");
    }

    /* Content order must be monotonic: a direction bug shows up here even when
     * the map check is fooled by a coincidence. */
    {
        int monotonic = 1;
        for (int y = 1; y < H; y++)
            if (fb[(size_t)y * W] < fb[(size_t)(y - 1) * W]) monotonic = 0;
        ok(monotonic, "rows stay in order, top to bottom");
    }

    printf("\n=== refuses what it cannot expand ===\n");
    {
        static uint16_t g[(size_t)W * H];
        paint(g, 0, H);
        uint16_t before = g[0];
        md32x_fullscreen_expand(g, 0, H);           /* already full */
        ok(g[0] == before, "a 240-line frame is left alone");

        paint(g, 8, 224);
        md32x_fullscreen_expand(g, -1, 224);        /* bad top */
        ok(g[0] == 0xDEAD, "a negative top is refused, buffer untouched");

        paint(g, 8, 224);
        md32x_fullscreen_expand(g, 8, 0);           /* empty */
        ok(g[0] == 0xDEAD, "an empty rect is refused, buffer untouched");

        paint(g, 8, 224);
        md32x_fullscreen_expand(g, 100, 224);       /* runs off the bottom */
        ok(g[0] == 0xDEAD, "a rect past the bottom is refused, buffer untouched");

        md32x_fullscreen_expand(NULL, 8, 224);
        ok(1, "a NULL buffer does not crash");
    }

    printf("\n=== other content rects a game can report ===\n");
    {
        static uint16_t g[(size_t)W * H];
        paint(g, 0, 224); md32x_fullscreen_expand(g, 0, 224);
        check_map(g, 0, 224, "top-aligned 224 expands correctly");
        paint(g, 16, 192); md32x_fullscreen_expand(g, 16, 192);
        check_map(g, 16, 192, "a 192-line rect expands correctly");
        paint(g, 8, 239); md32x_fullscreen_expand(g, 8, 239 - 8);
        check_map(g, 8, 231, "a 231-line rect (one row short) expands correctly");
    }

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
