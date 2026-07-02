/* linux/vb — headless host harness that mirrors the DEVICE Virtual Boy build as
 * closely as possible:
 *   - the SAME red-viper sources the firmware compiles (Makefile VB_C_SOURCES),
 *   - the SAME -DGNW_VB_DEVICE ifdef branches (single-player RAM regions through
 *     vb_dev_calloc, ROM pointer set externally exactly like app_main_vb),
 *   - the SAME software renderer (video_soft.cpp) and the SAME vb_blit conversion
 *     (384x224 2-bit column-major -> 320x240 RGB565 red, letterboxed) as main_vb.c.
 *
 * Headless: runs N frames with a fixed deterministic input script, prints one
 * FNV-1a hash line per frame for the 320x240 output, and dumps PPM snapshots.
 * Purpose: pixel-exact A/B of renderer changes (e.g. the tileCache shrink) on
 * REAL games before flashing — build two binaries that differ only in
 * video_soft.cpp and diff their stdout / PPMs.
 *
 * Usage: retro-go-vb <rom.vb> <frames> [ppm_every]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "vb_dsp.h"
#include "v810_mem.h"
#include "vb_set.h"

/* tDSPCACHE normally lives in the GLES2 renderer video.c, which we don't build;
 * the software renderer (video_soft.cpp) uses it — same trick as device main_vb.c. */
VB_DSPCACHE tDSPCACHE;
extern unsigned int vb_rom_mask;   /* v810_mem.c (power-of-2 ROM mirror mask) */

/* ---- device-glue stubs (host equivalents of the main_vb.c firmware hooks) ---- */
void *vb_dev_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
void *vb_dev_malloc(size_t size) { return malloc(size); }
/* VSU sound: irrelevant for pixel A/B — both variants use the same no-op. */
void sound_update(uint32_t cycles) { (void)cycles; }
void sound_write(int addr, uint16_t val) { (void)addr; (void)val; }
/* replay (rewind) infra is device/3DS save tooling; the core only pokes reset. */
void replay_reset(bool with_sram) { (void)with_sram; }
void replay_init(void) {}
/* GPU framebuffer readback is 3DS-only; the software renderer composites straight
 * into DISPLAY_RAM — identical no-op stub to device main_vb.c. */
void video_download_vip(int drawn_fb) { (void)drawn_fb; }

/* ---- identical to main_vb.c ---- */
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

/* vb_blit, verbatim from main_vb.c (device), writing to s_fb instead of the LCD. */
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

    /* Same optimized conversion as device main_vb.c (pal565 + accumulators). */
    uint16_t pal565[4];
    for (int v = 0; v < 4; v++) {
        int b = bri[v] * 2;
        if (b > 255) b = 255;
        pal565[v] = (uint16_t)((b >> 3) << 11);
    }
    int sy_acc = 0, sy = 0;
    for (int ry = 0; ry < dst_h; ry++) {
        const uint16_t *col = vb_fb + (sy >> 3);
        int shift = (sy & 7) * 2;
        uint16_t *dst = out + (y0 + ry) * GW_LCD_WIDTH;
        int sx_acc = 0;
        const uint16_t *src = col;
        for (int dx = 0; dx < dst_w; dx++) {
            dst[dx] = pal565[(*src >> shift) & 3];
            sx_acc += 384;
            while (sx_acc >= dst_w) { sx_acc -= dst_w; src += 32; }
        }
        sy_acc += 224;
        while (sy_acc >= dst_h) { sy_acc -= dst_h; sy++; }
    }
}

/* Deterministic input script (identical for both A/B binaries): tap START twice
 * to get from the title into the game/demo, then a couple of A presses. */
static void input_script(int frame)
{
    uint32_t k = 0;
    /* Tap START for 16 frames every 250 frames (skips boot/IPD/warning screens and
     * starts a game), plus an A press mid-run. Identical in both A/B binaries. */
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

static void dump_ppm(const char *name, const uint16_t *fb)
{
    FILE *f = fopen(name, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", GW_LCD_WIDTH, GW_LCD_HEIGHT);
    for (int i = 0; i < GW_LCD_WIDTH * GW_LCD_HEIGHT; i++) {
        uint8_t rgb[3] = { (uint8_t)(((fb[i] >> 11) & 31) << 3), 0, 0 };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <rom.vb> <frames> [ppm_every]\n", argv[0]); return 2; }
    const char *rom_path = argv[1];
    int max_frames = atoi(argv[2]);
    int ppm_every  = (argc > 3) ? atoi(argv[3]) : 300;

    FILE *rf = fopen(rom_path, "rb");
    if (!rf) { fprintf(stderr, "rom open fail: %s\n", rom_path); return 2; }
    fseek(rf, 0, SEEK_END);
    long rom_len = ftell(rf);
    rewind(rf);
    unsigned char *rom = (unsigned char *)malloc(rom_len);
    if (fread(rom, 1, rom_len, rf) != (size_t)rom_len) { fprintf(stderr, "rom read fail\n"); return 2; }
    fclose(rf);

    /* Boot sequence: same order as app_main_vb (device). */
    setDefaults();
    is_multiplayer = false;
    v810_init();
    printf("[vb-host] regions DISP=%p SND=%p WRAM=%p GRAM=%p\n",
           (void *)vb_state->V810_DISPLAY_RAM.pmemory, (void *)vb_state->V810_SOUND_RAM.pmemory,
           (void *)vb_state->V810_VB_RAM.pmemory,      (void *)vb_state->V810_GAME_RAM.pmemory);

    V810_ROM1.pmemory  = rom;
    V810_ROM1.lowaddr  = 0x07000000;
    V810_ROM1.size     = (uint32_t)rom_len;
    V810_ROM1.highaddr = 0x07000000 + (uint32_t)rom_len - 1;
    V810_ROM1.off      = (size_t)rom - 0x07000000;
    vb_rom_mask        = (unsigned int)(rom_len - 1);
    tVBOpt.CRC32       = vb_rom_stamp(rom, (uint32_t)rom_len);
    printf("[vb-host] rom=%s len=%ld stamp=%08x\n", rom_path, rom_len, (unsigned)tVBOpt.CRC32);

    v810_reset();
    clearCache();
    /* Bootstrap normally done by red-viper's 3DS GUI (vb_gui.c game_load):
     * - the device/host port uses the SOFTWARE VIP, not the 3DS GPU default
     * - frametime starts 0 and is only re-set at the fb flip; the flip in turn
     *   requires a completed draw whose row progress divides by frametime —
     *   0 means the draw never completes and tDisplayedFB never flips (black). */
    tVBOpt.RENDERMODE = RM_CPUONLY;
    vb_state->tVIPREG.frametime = videoProcessingTime();

    uint32_t run_hash = 2166136261u;
    for (int frame = 0; frame < max_frames; frame++) {
        input_script(frame);
        v810_run();
        vb_blit();
        uint32_t h = fnv1a(s_fb, sizeof(s_fb));
        run_hash = (run_hash ^ h) * 16777619u;
        printf("f%05d fb=%08x\n", frame, h);
        if (frame % 100 == 0) {
            /* VIP liveness: is the game drawing at all, and is brightness set?
             * dram0/1 = checksums of the two left-eye framebuffers in DISPLAY_RAM. */
            printf("  vip XPCTRL=%04x XPSTTS=%04x DPCTRL=%04x draw=%d disp=%d dfb=%d tFrame=%d FRMCYC=%d INTENB=%04x INTPND=%04x ft=%d BRT=%d/%d/%d PC=%08x dram0=%08x dram1=%08x\n",
                   vb_state->tVIPREG.XPCTRL, vb_state->tVIPREG.XPSTTS, vb_state->tVIPREG.DPCTRL,
                   (int)vb_state->tVIPREG.drawing, (int)vb_state->tVIPREG.displaying,
                   (int)vb_state->tVIPREG.tDisplayedFB, (int)vb_state->tVIPREG.tFrame,
                   (int)vb_state->tVIPREG.FRMCYC,
                   vb_state->tVIPREG.INTENB, vb_state->tVIPREG.INTPND,
                   vb_state->tVIPREG.frametime,
                   (int)vb_state->tVIPREG.BRTA, (int)vb_state->tVIPREG.BRTB, (int)vb_state->tVIPREG.BRTC,
                   (unsigned)vb_state->v810_state.PC,
                   fnv1a(vb_state->V810_DISPLAY_RAM.pmemory, 0x6000),
                   fnv1a(vb_state->V810_DISPLAY_RAM.pmemory + 0x8000, 0x6000));
        }
        if (ppm_every > 0 && frame > 0 && frame % ppm_every == 0) {
            char nm[64]; snprintf(nm, sizeof nm, "vb_f%05d.ppm", frame);
            dump_ppm(nm, s_fb);
        }
    }
    dump_ppm("vb_end.ppm", s_fb);
    printf("[vb-host] done %d frames RUNHASH=%08x PC=%08x\n",
           max_frames, run_hash, (unsigned)vb_state->v810_state.PC);
    return 0;
}
