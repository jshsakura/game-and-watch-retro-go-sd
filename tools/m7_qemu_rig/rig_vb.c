/* Virtual Boy on the M7 QEMU rig: the linux/vb harness driver, bare-metal.
 *
 * Same core sources, same GNW_VB_DEVICE branches, same software renderer and
 * vb_blit as the device — but running as a REAL ARMv7-M instruction stream,
 * so a CMSDK-timer delta under -icount shift=0 is an executed-instruction
 * count. Prints per-window instructions/frame plus the same frame hashes the
 * host harness prints, so a run can be cross-checked against linux/vb output
 * (same ROM, same input script => same hashes).
 *
 * The ROM is linked in (objcopy -I binary): symbols _binary_rom_vb_start/end.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "vb_dsp.h"
#include "v810_mem.h"
#include "vb_set.h"

#ifndef RIG_FRAMES
#define RIG_FRAMES 3000
#endif
#define RIG_WINDOW 200 /* print a ledger line every N frames */

VB_DSPCACHE tDSPCACHE;
extern unsigned int vb_rom_mask;

extern unsigned char _binary_rom_vb_start[];
extern unsigned char _binary_rom_vb_end[];

/* rig_runtime.c */
void rig_timer_init(void);
uint32_t rig_timer_now(void);
uint32_t rig_calibrate(uint32_t n);

/* ---- device-glue stubs, identical to linux/vb/main.c ---- */
void *vb_dev_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
void *vb_dev_malloc(size_t size) { return malloc(size); }
void sound_update(uint32_t cycles) { (void)cycles; }
void sound_write(int addr, uint16_t val) { (void)addr; (void)val; }
void replay_reset(bool with_sram) { (void)with_sram; }
void replay_init(void) {}
void video_download_vip(int drawn_fb) { (void)drawn_fb; }

static uint32_t vb_rom_stamp(const unsigned char *rom, uint32_t len)
{
    uint32_t h = 2166136261u ^ len;
    uint32_t step = len > 4096 ? len / 4096 : 1;
    for (uint32_t i = 0; i < len; i += step) { h ^= rom[i]; h *= 16777619u; }
    return h;
}

#define GW_LCD_WIDTH  320
#define GW_LCD_HEIGHT 240
static uint16_t s_fb[GW_LCD_WIDTH * GW_LCD_HEIGHT];

/* vb_blit, verbatim from linux/vb/main.c (itself verbatim from main_vb.c). */
static void vb_blit(void)
{
    int dfb = vb_state->tVIPREG.tDisplayedFB;

    if (vb_state->tVIPREG.tFrame == 0 && !vb_state->tVIPREG.drawing &&
        (vb_state->tVIPREG.XPCTRL & XPEN)) {
        if (tDSPCACHE.CharCacheInvalid) update_texture_cache_soft();
        video_soft_render(!dfb);
        tDSPCACHE.CharCacheInvalid = false;
        memset(tDSPCACHE.CharacterCache, 0, sizeof(tDSPCACHE.CharacterCache));
    }

    const uint16_t *vb_fb =
        (const uint16_t *)(vb_state->V810_DISPLAY_RAM.pmemory + 0x8000 * dfb);

    int bri[4];
    bri[0] = 0;
    bri[1] = vb_state->tVIPREG.BRTA;
    bri[2] = vb_state->tVIPREG.BRTB;
    bri[3] = vb_state->tVIPREG.BRTA + vb_state->tVIPREG.BRTB + vb_state->tVIPREG.BRTC;

    uint16_t *out = s_fb;
    const int dst_w = GW_LCD_WIDTH;
    const int dst_h = GW_LCD_WIDTH * 224 / 384;
    const int y0    = (GW_LCD_HEIGHT - dst_h) / 2;

    memset(out, 0, (size_t)GW_LCD_WIDTH * GW_LCD_HEIGHT * sizeof(uint16_t));

    uint16_t pal565[4];
    for (int v = 0; v < 4; v++) {
        int b = bri[v] * 2;
        if (b > 255) b = 255;
        pal565[v] = (uint16_t)((b >> 3) << 11);
    }
    /* Cache-friendly transpose-scale — see main_vb.c for the rationale. Column-
     * outer so each source column's cache lines are read once and reused down
     * the output rows, instead of a 64-byte hop per source pixel. Bit-for-bit
     * identical to the old output-driven loop (same Bresenham maps). */
    static uint16_t col_word[GW_LCD_WIDTH];
    static uint8_t  row_woff[GW_LCD_HEIGHT];
    static uint8_t  row_shift[GW_LCD_HEIGHT];
    static int      maps_ready = 0;
    if (!maps_ready) {
        int sx_acc = 0, col = 0;
        for (int dx = 0; dx < dst_w; dx++) {
            col_word[dx] = (uint16_t)(col * 32);
            sx_acc += 384;
            while (sx_acc >= dst_w) { sx_acc -= dst_w; col++; }
        }
        int sy_acc = 0, sy = 0;
        for (int ry = 0; ry < dst_h; ry++) {
            row_woff[ry]  = (uint8_t)(sy >> 3);
            row_shift[ry] = (uint8_t)((sy & 7) * 2);
            sy_acc += 224;
            while (sy_acc >= dst_h) { sy_acc -= dst_h; sy++; }
        }
        maps_ready = 1;
    }
    for (int dx = 0; dx < dst_w; dx++) {
        const uint16_t *column = vb_fb + col_word[dx];
        uint16_t *dst = out + y0 * GW_LCD_WIDTH + dx;
        for (int ry = 0; ry < dst_h; ry++) {
            *dst = pal565[(column[row_woff[ry]] >> row_shift[ry]) & 3];
            dst += GW_LCD_WIDTH;
        }
    }
}

/* Deterministic input script — MUST stay identical to linux/vb/main.c so the
 * two rigs produce comparable hashes. */
static void input_script(int frame)
{
    uint32_t k = 0;
    if (frame >= 250 && (frame % 250) < 16)
        k |= VB_KEY_START;
    if (frame >= 1100 && frame < 1116)
        k |= VB_KEY_A;
    vb_state->tHReg.SLB = (uint8_t)(k & 0xFF);
    vb_state->tHReg.SHB = (uint8_t)((k >> 8) & 0xFF);
}

static uint32_t fnv1a(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    while (len--) { h ^= *p++; h *= 16777619u; }
    return h;
}

int main(void)
{
    unsigned char *rom = _binary_rom_vb_start;
    uint32_t rom_len = (uint32_t)(_binary_rom_vb_end - _binary_rom_vb_start);

    rig_timer_init();
    /* Calibrate ticks -> instructions: the loop body is exactly 3 insns. */
    uint32_t cal_ticks = rig_calibrate(1000000);
    /* insns_per_tick scaled by 1000 to keep integer math honest. */
    uint32_t ipt_x1000 = (uint32_t)((3000000ull * 1000ull) / (cal_ticks ? cal_ticks : 1));
    printf("[vb-qemu] cal: 3.0M insns = %lu ticks -> %lu.%03lu insn/tick\n",
           (unsigned long)cal_ticks,
           (unsigned long)(ipt_x1000 / 1000), (unsigned long)(ipt_x1000 % 1000));

    setDefaults();
    is_multiplayer = false;
    v810_init();

    V810_ROM1.pmemory  = rom;
    V810_ROM1.lowaddr  = 0x07000000;
    V810_ROM1.size     = rom_len;
    V810_ROM1.highaddr = 0x07000000 + rom_len - 1;
    V810_ROM1.off      = (size_t)rom - 0x07000000;
    vb_rom_mask        = rom_len - 1;
    tVBOpt.CRC32       = vb_rom_stamp(rom, rom_len);
    printf("[vb-qemu] rom len=%lu stamp=%08x frames=%d\n",
           (unsigned long)rom_len, (unsigned)tVBOpt.CRC32, RIG_FRAMES);

    v810_reset();
    clearCache();
    tVBOpt.RENDERMODE = RM_CPUONLY;
    vb_state->tVIPREG.frametime = videoProcessingTime();

    uint32_t run_hash = 2166136261u;
    uint64_t win_emu_ticks = 0, win_blit_ticks = 0;
    uint64_t tot_emu_ticks = 0, tot_blit_ticks = 0;

    for (int frame = 0; frame < RIG_FRAMES; frame++) {
        input_script(frame);
        uint32_t t0 = rig_timer_now();
        v810_run();
        uint32_t t1 = rig_timer_now();
        vb_blit();
        uint32_t t2 = rig_timer_now();
        win_emu_ticks += (uint32_t)(t1 - t0);
        win_blit_ticks += (uint32_t)(t2 - t1);

        uint32_t h = fnv1a(s_fb, sizeof(s_fb));
        run_hash = (run_hash ^ h) * 16777619u;

        if ((frame + 1) % RIG_WINDOW == 0) {
            uint64_t emu_i  = win_emu_ticks * ipt_x1000 / 1000 / RIG_WINDOW;
            uint64_t blit_i = win_blit_ticks * ipt_x1000 / 1000 / RIG_WINDOW;
            printf("w%05d emu=%lu blit=%lu insn/frame fb=%08x\n",
                   frame + 1, (unsigned long)emu_i, (unsigned long)blit_i, (unsigned)h);
            tot_emu_ticks += win_emu_ticks;
            tot_blit_ticks += win_blit_ticks;
            win_emu_ticks = win_blit_ticks = 0;
        }
    }

    uint64_t frames = (RIG_FRAMES / RIG_WINDOW) * RIG_WINDOW;
    if (frames == 0) frames = 1;
    printf("[vb-qemu] done %d frames RUNHASH=%08x avg emu=%lu blit=%lu insn/frame\n",
           RIG_FRAMES, (unsigned)run_hash,
           (unsigned long)(tot_emu_ticks * ipt_x1000 / 1000 / frames),
           (unsigned long)(tot_blit_ticks * ipt_x1000 / 1000 / frames));
    return 0;
}
