/*
 * CPS-1 PC host harness -- skeleton (Phase 2 of the CPS-1 feasibility
 * protocol, docs/CPS1_SENIOR_TRICKS_ANALYSIS.md section 4), now integrating
 * every piece built since: 68000 interpreter+recompiler with a real WRAM/
 * OBJ-RAM/palette-RAM/BG-tilemap/sound-command bus, a 3-layer BG renderer
 * + host compositor, and a sound HLE mixer -- all against synthetic ROM/
 * scene data (see Core/Src/porting/cps1/cps1_core.c), since no real CPS-1
 * ROM exists yet.
 *
 *   ./build/retro-go-cps1 [--frames=N] [--engine=interpreter|recompiler|diff] [--dump-ppm]
 *   ./build/retro-go-cps1 --profile [--frames=N]
 *
 * --profile times cps1_core_run_frame_device_cost() (CPU+SCROLL3+sprites
 * only -- the cheat-8 device-realistic path, NOT the full host-compositor
 * pipeline) on THIS x86 host. Wall-clock ms/frame here says nothing about
 * device fps (different ISA, no cache/wait-state model) -- it is a sanity
 * check that the reference path runs in a sane amount of time, not a fps
 * verdict. The QEMU M7 rig (tools/m7_qemu_rig/run_cps1.sh) is what measures
 * instructions/frame against the device's actual 340MHz/60fps budget.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

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
    uint32_t cpu_hash = 0;

    for (int f = 0; f < frames; f++) {
        cps1_core_run_frame(CPS1_ENGINE_INTERPRETER);
        cps1_core_run_frame(CPS1_ENGINE_RECOMPILER);

        uint32_t hi = fb_crc32(cps1_core_get_framebuffer(CPS1_ENGINE_INTERPRETER));
        uint32_t hr = fb_crc32(cps1_core_get_framebuffer(CPS1_ENGINE_RECOMPILER));
        uint32_t ci = cps1_core_cpu_state_hash(CPS1_ENGINE_INTERPRETER);
        uint32_t cr = cps1_core_cpu_state_hash(CPS1_ENGINE_RECOMPILER);
        if (hi != hr || ci != cr) {
            fprintf(stderr,
                    "[cps1] MISMATCH frame %d: fb interpreter=%08x recompiler=%08x | "
                    "cpu interpreter=%08x recompiler=%08x\n",
                    f, hi, hr, ci, cr);
            mismatches++;
        }
        run_hash = hi;
        cpu_hash = ci;
    }

    if (mismatches) {
        fprintf(stderr, "[cps1] FAIL: %d/%d frames mismatched\n", mismatches, frames);
        return 1;
    }

    uint32_t illegal = cps1_core_cpu_illegal_count(CPS1_ENGINE_INTERPRETER);
    if (illegal) {
        fprintf(stderr, "[cps1] FAIL: interpreter hit %u unimplemented opcode(s)\n", illegal);
        return 1;
    }

    printf("[cps1] OK: %d frames bit-identical, RUNHASH=%08x CPUHASH=%08x\n",
           frames, run_hash, cpu_hash);
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

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int run_profile(int frames)
{
    cps1_core_reset(CPS1_ENGINE_INTERPRETER);

    /* Warm up the tile cache so the first measured frames aren't paying a
     * one-time cold-cache cost that a real running game wouldn't repeat. */
    for (int f = 0; f < 5; f++)
        cps1_core_run_frame_device_cost(CPS1_ENGINE_INTERPRETER);

    double total_ms = 0.0, min_ms = 1e9, max_ms = 0.0;
    for (int f = 0; f < frames; f++) {
        double t0 = now_ms();
        cps1_core_run_frame_device_cost(CPS1_ENGINE_INTERPRETER);
        double dt = now_ms() - t0;
        total_ms += dt;
        if (dt < min_ms) min_ms = dt;
        if (dt > max_ms) max_ms = dt;
    }

    double avg_ms = total_ms / frames;
    printf("[cps1-profile] x86 HOST wall-clock, device_cost path (CPU+SCROLL3+sprites), %d frames\n",
           frames);
    printf("[cps1-profile] avg=%.4f ms/frame min=%.4f max=%.4f (16.6ms budget line is NOT a device fps verdict here -- see file header)\n",
           avg_ms, min_ms, max_ms);
    return 0;
}

int main(int argc, char **argv)
{
    int frames = 60;
    int dump_ppm = 0;
    int profile = 0;
    const char *engine_arg = "diff";

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--frames=", 9))
            frames = atoi(argv[i] + 9);
        else if (!strncmp(argv[i], "--engine=", 9))
            engine_arg = argv[i] + 9;
        else if (!strcmp(argv[i], "--dump-ppm"))
            dump_ppm = 1;
        else if (!strcmp(argv[i], "--profile"))
            profile = 1;
        else
            fprintf(stderr, "cps1: ignoring unrecognized argument '%s'\n", argv[i]);
    }

    printf("[cps1] integrated harness: 68000+bus (WRAM/OBJ/palette/BG-tilemap/sound-cmd), "
           "3-layer BG+compositor, sound HLE -- all synthetic scene/ROM data, see "
           "docs/CPS1_FEASIBILITY.md\n");

    if (profile) {
        printf("[cps1] profile mode, frames=%d\n", frames);
        return run_profile(frames);
    }

    printf("[cps1] frames=%d engine=%s\n", frames, engine_arg);

    if (!strcmp(engine_arg, "diff"))
        return run_diff(frames, dump_ppm);
    if (!strcmp(engine_arg, "recompiler"))
        return run_single(CPS1_ENGINE_RECOMPILER, frames, dump_ppm);
    return run_single(CPS1_ENGINE_INTERPRETER, frames, dump_ppm);
}
