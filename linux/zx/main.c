/* Minimal headless host harness for the ZX Spectrum (floooh/chips zx.h) core.
 * Validates the core + .z80 loading + video deterministically on PC before any
 * device integration. Build: make -f Makefile.zx ; run: ./build/zx <game.z80> <frames>
 * Needs 48.rom in the cwd. Dumps zx_frame.ppm. */
#define CHIPS_IMPL
#define CHIPS_ASSERT(c) ((void)0)
#include "chips/chips_common.h"
#include "chips/z80.h"
#include "chips/beeper.h"
#include "chips/ay38910.h"
#include "chips/kbd.h"
#include "chips/mem.h"
#include "chips/clk.h"
#include "chips/zx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static zx_t   zx;
static uint8_t rom48[16384];
static uint8_t game[512 * 1024];

static void audio_cb(const float *s, int n, void *u) { (void)s; (void)n; (void)u; }

int main(int argc, char **argv)
{
    const char *z80path = (argc > 1) ? argv[1] : NULL;
    int frames = (argc > 2) ? atoi(argv[2]) : 200;

    FILE *f = fopen("48.rom", "rb");
    if (!f) { printf("[zx] missing 48.rom in cwd\n"); return 1; }
    if (fread(rom48, 1, 16384, f) != 16384) { printf("[zx] 48.rom not 16K\n"); fclose(f); return 1; }
    fclose(f);

    zx_desc_t desc = {0};
    desc.type           = ZX_TYPE_48K;
    desc.joystick_type  = ZX_JOYSTICKTYPE_KEMPSTON;
    desc.audio.callback.func = audio_cb;
    desc.audio.num_samples   = 128;
    desc.audio.sample_rate   = 22050;
    desc.audio.beeper_volume = 0.5f;
    desc.audio.ay_volume     = 0.5f;
    desc.roms.zx48k.ptr  = rom48;
    desc.roms.zx48k.size = 16384;
    zx_init(&zx, &desc);

    if (z80path) {
        FILE *g = fopen(z80path, "rb");
        if (g) {
            size_t n = fread(game, 1, sizeof(game), g);
            fclose(g);
            bool ok = zx_quickload(&zx, (chips_range_t){ .ptr = game, .size = n });
            printf("[zx] quickload %s -> %s (%zu bytes)\n", z80path, ok ? "OK" : "FAIL", n);
        } else {
            printf("[zx] cannot open %s\n", z80path);
        }
    }

    for (int i = 0; i < frames; i++)
        zx_exec(&zx, 19968);   /* ~one 50Hz PAL frame */

    chips_display_info_t di = zx_display_info(&zx);
    const uint32_t *pal = (const uint32_t *)di.palette.ptr;
    FILE *p = fopen("zx_frame.ppm", "wb");
    fprintf(p, "P6\n%d %d\n255\n", ZX_DISPLAY_WIDTH, ZX_DISPLAY_HEIGHT);
    for (int y = 0; y < ZX_DISPLAY_HEIGHT; y++)
        for (int x = 0; x < ZX_DISPLAY_WIDTH; x++) {
            uint8_t idx = zx.fb[y * ZX_FRAMEBUFFER_WIDTH + x];
            uint32_t c = pal[idx & 15];          /* 0xAABBGGRR */
            fputc(c & 0xFF, p); fputc((c >> 8) & 0xFF, p); fputc((c >> 16) & 0xFF, p);
        }
    fclose(p);
    printf("[zx] wrote zx_frame.ppm %dx%d after %d frames\n", ZX_DISPLAY_WIDTH, ZX_DISPLAY_HEIGHT, frames);
    return 0;
}
