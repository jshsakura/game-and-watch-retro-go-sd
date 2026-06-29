/* Minimal headless host harness for the Commodore 64 (floooh/chips c64.h) core.
 * Validates the core + ROM boot + .prg quickload + video deterministically on PC
 * before any device integration.
 *   build: make -f Makefile.c64
 *   run:   ./build/c64 [game.prg] [frames]
 * Needs the three C64 ROM dumps in the cwd (NOT committed — supply your own):
 *   chargen.bin (4096) / basic.bin (8192) / kernal.bin (8192)
 * Dumps c64_frame.ppm. With a .prg it warms up, then types RUN<Return>. */
#include <assert.h>   /* m6502.h uses a bare assert() in one unreached opcode slot */
#define CHIPS_IMPL
#define CHIPS_ASSERT(c) ((void)0)
#include "chips/chips_common.h"
#include "chips/m6502.h"
#include "chips/m6526.h"
#include "chips/m6522.h"
#include "chips/m6569.h"
#include "chips/m6581.h"
#include "chips/kbd.h"
#include "chips/mem.h"
#include "chips/clk.h"
#include "chips/c1530.h"
#include "chips/c1541.h"
#include "chips/c64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static c64_t   c64;
static uint8_t rom_char[4096];
static uint8_t rom_basic[8192];
static uint8_t rom_kernal[8192];
static uint8_t prg[512 * 1024];

#define FRAME_US     20000   /* one PAL frame at 50 Hz */
#define WARMUP_FRAMES 150     /* ~3 s to reach the READY. prompt */

static void audio_cb(const float *s, int n, void *u) { (void)s; (void)n; (void)u; }

static int load_rom(const char *name, uint8_t *dst, size_t want)
{
    FILE *f = fopen(name, "rb");
    if (!f) { printf("[c64] missing %s in cwd\n", name); return 0; }
    size_t n = fread(dst, 1, want, f);
    fclose(f);
    if (n != want) { printf("[c64] %s wrong size: got %zu want %zu\n", name, n, want); return 0; }
    return 1;
}

/* type one ASCII char as a press+release spanning a couple of frames */
static void type_char(uint8_t ascii)
{
    c64_key_down(&c64, ascii);
    c64_exec(&c64, FRAME_US);
    c64_exec(&c64, FRAME_US);
    c64_key_up(&c64, ascii);
    c64_exec(&c64, FRAME_US);
}

int main(int argc, char **argv)
{
    /* usage:
     *   c64                         -> boot to READY, dump screen
     *   c64 --type "BASIC LINE"     -> boot, type the line + Return, run, dump
     *   c64 game.prg [frames]       -> boot, quickload .prg, type RUN, dump   */
    const char *typestr = NULL, *prgpath = NULL;
    int frames = WARMUP_FRAMES;
    if (argc > 2 && strcmp(argv[1], "--type") == 0) {
        typestr = argv[2];
    } else if (argc > 1) {
        prgpath = argv[1];
        if (argc > 2) frames = atoi(argv[2]);
    }

    if (!load_rom("chargen.bin", rom_char,  sizeof(rom_char))  ||
        !load_rom("basic.bin",   rom_basic, sizeof(rom_basic)) ||
        !load_rom("kernal.bin",  rom_kernal, sizeof(rom_kernal)))
        return 1;

    c64_desc_t desc = {0};
    desc.joystick_type        = C64_JOYSTICKTYPE_NONE;
    desc.audio.callback.func  = audio_cb;
    desc.audio.num_samples    = 128;
    desc.audio.sample_rate    = 22050;
    desc.roms.chars.ptr  = rom_char;   desc.roms.chars.size  = sizeof(rom_char);
    desc.roms.basic.ptr  = rom_basic;  desc.roms.basic.size  = sizeof(rom_basic);
    desc.roms.kernal.ptr = rom_kernal; desc.roms.kernal.size = sizeof(rom_kernal);
    c64_init(&c64, &desc);

    /* warm up to the BASIC READY. prompt */
    for (int i = 0; i < frames; i++)
        c64_exec(&c64, FRAME_US);

    if (typestr) {
        for (const char *c = typestr; *c; c++) type_char((uint8_t)*c);
        type_char('\r');
        for (int i = 0; i < 60; i++) c64_exec(&c64, FRAME_US);
        printf("[c64] typed: %s\n", typestr);
    }

    if (prgpath) {
        FILE *g = fopen(prgpath, "rb");
        if (g) {
            size_t n = fread(prg, 1, sizeof(prg), g);
            fclose(g);
            bool ok = c64_quickload(&c64, (chips_range_t){ .ptr = prg, .size = n });
            printf("[c64] quickload %s -> %s (%zu bytes, load $%02X%02X)\n",
                   prgpath, ok ? "OK" : "FAIL", n, n > 1 ? prg[1] : 0, n > 0 ? prg[0] : 0);
            if (ok) {
                const char *cmd = "RUN\r";
                for (const char *c = cmd; *c; c++) type_char((uint8_t)*c);
                for (int i = 0; i < 120; i++) c64_exec(&c64, FRAME_US);  /* let it run */
            }
        } else {
            printf("[c64] cannot open %s\n", prgpath);
        }
    }

    chips_display_info_t di = c64_display_info(&c64);
    const uint32_t *pal = (const uint32_t *)di.palette.ptr;
    const uint8_t  *fb  = (const uint8_t *)di.frame.buffer.ptr;
    int w = di.frame.dim.width, h = di.frame.dim.height;
    FILE *p = fopen("c64_frame.ppm", "wb");
    fprintf(p, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = pal[fb[y * w + x]];     /* 0xAABBGGRR */
            fputc(c & 0xFF, p); fputc((c >> 8) & 0xFF, p); fputc((c >> 16) & 0xFF, p);
        }
    fclose(p);
    printf("[c64] wrote c64_frame.ppm %dx%d after %d frames%s\n",
           w, h, frames, prgpath ? " (+prg/RUN)" : "");
    return 0;
}
