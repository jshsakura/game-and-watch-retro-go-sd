/*
 * CPS-1 PC host harness -- skeleton (Phase 2 of the CPS-1 feasibility
 * protocol, docs/CPS1_SENIOR_TRICKS_ANALYSIS.md section 4). Headless: there
 * is nothing to look at yet (see Core/Src/porting/cps1/cps1_core.c, a stub).
 * Its job is the interpreter-vs-recompiler bit-identical diff loop the real
 * cores will plug into once they exist -- and to fail loudly (mismatched
 * checksums) the day one of them diverges from the other.
 *
 *   ./build/retro-go-cps1 [--frames=N] [--engine=interpreter|recompiler|diff] [--dump-ppm]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "cps1_core.h"
#include "crc32.h"

static uint32_t fb_crc32(const uint16_t *fb)
{
    return crc32_le(0, (const unsigned char *)fb,
                     (unsigned int)(CPS1_FB_WIDTH * CPS1_FB_HEIGHT * sizeof(uint16_t)));
}

static void write_ppm(const char *path, const uint16_t *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cps1: cannot open '%s' for write\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", CPS1_FB_WIDTH, CPS1_FB_HEIGHT);
    for (int i = 0; i < CPS1_FB_WIDTH * CPS1_FB_HEIGHT; i++) {
        uint16_t px = fb[i];
        uint8_t r = (uint8_t)(((px >> 11) & 0x1Fu) * 255u / 31u);
        uint8_t g = (uint8_t)(((px >> 5) & 0x3Fu) * 255u / 63u);
        uint8_t b = (uint8_t)((px & 0x1Fu) * 255u / 31u);
        fputc(r, f);
        fputc(g, f);
        fputc(b, f);
    }
    fclose(f);
    printf("[cps1] wrote %s\n", path);
}

static int run_diff(int frames, int dump_ppm)
{
    cps1_core_reset(CPS1_ENGINE_INTERPRETER);
    cps1_core_reset(CPS1_ENGINE_RECOMPILER);

    int mismatches = 0;
    uint32_t run_hash = 0;

    for (int f = 0; f < frames; f++) {
        cps1_core_run_frame(CPS1_ENGINE_INTERPRETER);
        cps1_core_run_frame(CPS1_ENGINE_RECOMPILER);

        uint32_t hi = fb_crc32(cps1_core_get_framebuffer(CPS1_ENGINE_INTERPRETER));
        uint32_t hr = fb_crc32(cps1_core_get_framebuffer(CPS1_ENGINE_RECOMPILER));
        if (hi != hr) {
            fprintf(stderr, "[cps1] MISMATCH frame %d: interpreter=%08x recompiler=%08x\n",
                    f, hi, hr);
            mismatches++;
        }
        run_hash = hi;
    }

    if (mismatches) {
        fprintf(stderr, "[cps1] FAIL: %d/%d frames mismatched\n", mismatches, frames);
        return 1;
    }

    printf("[cps1] OK: %d frames bit-identical, RUNHASH=%08x\n", frames, run_hash);
    if (dump_ppm)
        write_ppm("build/cps1_last_frame.ppm", cps1_core_get_framebuffer(CPS1_ENGINE_INTERPRETER));
    return 0;
}

static int run_single(cps1_engine_kind_t engine, int frames, int dump_ppm)
{
    cps1_core_reset(engine);
    uint32_t run_hash = 0;
    for (int f = 0; f < frames; f++) {
        cps1_core_run_frame(engine);
        run_hash = fb_crc32(cps1_core_get_framebuffer(engine));
    }
    printf("[cps1] done %d frames RUNHASH=%08x\n", frames, run_hash);
    if (dump_ppm)
        write_ppm("build/cps1_last_frame.ppm", cps1_core_get_framebuffer(engine));
    return 0;
}

int main(int argc, char **argv)
{
    int frames = 60;
    int dump_ppm = 0;
    const char *engine_arg = "diff";

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--frames=", 9))
            frames = atoi(argv[i] + 9);
        else if (!strncmp(argv[i], "--engine=", 9))
            engine_arg = argv[i] + 9;
        else if (!strcmp(argv[i], "--dump-ppm"))
            dump_ppm = 1;
        else
            fprintf(stderr, "cps1: ignoring unrecognized argument '%s'\n", argv[i]);
    }

    printf("[cps1] STUB harness -- no real 68000/Z80/PPU/sound yet, see docs/CPS1_FEASIBILITY.md\n");
    printf("[cps1] frames=%d engine=%s\n", frames, engine_arg);

    if (!strcmp(engine_arg, "diff"))
        return run_diff(frames, dump_ppm);
    if (!strcmp(engine_arg, "recompiler"))
        return run_single(CPS1_ENGINE_RECOMPILER, frames, dump_ppm);
    return run_single(CPS1_ENGINE_INTERPRETER, frames, dump_ppm);
}
