/* Compile twice with/without GNW_MD_NO_BLANK_RUN and compare the full output.
 * Includes the real renderer so pixels, priority lists and status flags are
 * checked at scroll/wrap boundaries, including shadow and forced drawing.
 * Link with --gc-sections to discard unused emulator entry points. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../external/picodrive/pico/draw.c"

struct Pico Pico;
struct PicoMem PicoMem;
PicoInterface PicoIn;

static uint32_t rng = 0x32abcdef;
static uint32_t next_random(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

int main(void)
{
    static const int scrolls[] = {-257, -17, -8, -1, 0, 1, 7, 8, 15, 257};
    unsigned char pixels[400];
    u32 hcache[83];
    unsigned cases = 0;

    for (int interlace = 0; interlace < 2; interlace++)
    for (int width = 32; width <= 128; width *= 2)
    for (int shape = 0; shape < 24; shape++)
    for (int flags = 0; flags < 8; flags++)
    for (unsigned scroll = 0; scroll < sizeof(scrolls)/sizeof(scrolls[0]); scroll++)
    for (int skip = 0; skip <= 8; skip += 4)
    for (int line = 0; line < 16; line++) {
        /* Blank tile 1; other tiles exercise blank rows, asymmetric pixels,
         * flips and both priority/palette bits. Recreate VRAM every case. */
        memset(&PicoMem, 0, sizeof(PicoMem));
        for (unsigned i = interlace ? 64 : 32; i < 2048; i++)
            PicoMem.vram[i] = (next_random() & 3) ? next_random() : 0;
        if (shape == 22)
            PicoMem.vram[interlace ? 32 : 16] = 0x1234;
        u16 common = 1 | ((shape & 7) << 13) | ((shape & 8) ? 0x1800 : 0);
        int nametab = 0x4000 + (shape & 1);
        for (int col = 0; col < width; col++) {
            u16 code = common;
            if (shape >= 16 && shape < 20 && col % (shape - 14) == 0)
                code = (next_random() & 0xf800) | (2 + next_random() % 30);
            /* One different tile at each position in an eight-tile run. */
            else if (shape >= 8 && shape < 16 && col == 8 + shape - 8)
                code = 0x8002;
            else if (shape == 20)
                code = 1 | ((col & 3) << 13) | ((col & 1) ? 0x800 : 0);
            else if (shape == 21)
                code = 1 | ((col & 1) ? 0x8000 : 0);
            else if (shape == 22)
                code = 1 | ((col & 1) ? 0x1000 : 0);
            else if (shape == 23)
                code = col >= 40 ? 2 : 1;
            PicoMem.vram[nametab + col] = code;
        }
        memset(pixels, 0xa5, sizeof(pixels));
        memset(hcache, 0x5a, sizeof(hcache));
        Pico.est.HighCol = pixels + 32;
        Pico.est.rendstatus = 0x12340000;
        struct TileStrip ts = {nametab, line, scrolls[scroll], width - 1,
                               hcache, (shape & 1) ? 32 : 40};
        int lflags = (flags & 1 ? LF_SH : 0) | (flags & 2 ? LF_LINE : 0);
        if (flags & 4) {
            if (interlace) DrawStripInterlaceForced(&ts, lflags, skip);
            else DrawStripForced(&ts, lflags, skip);
        } else {
            if (interlace) DrawStripInterlace(&ts, lflags, skip);
            else DrawStrip(&ts, lflags, skip);
        }
        if (fwrite(pixels, sizeof(pixels), 1, stdout) != 1 ||
            fwrite(hcache, sizeof(hcache), 1, stdout) != 1 ||
            fwrite(&Pico.est.rendstatus, sizeof(Pico.est.rendstatus), 1, stdout) != 1)
            return 1;
        cases++;
    }
    fprintf(stderr, "blank-run differential cases=%u\n", cases);
    return 0;
}
