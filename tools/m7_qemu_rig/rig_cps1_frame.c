/*
 * What does ONE REAL Tenchi wo Kurau II frame cost the renderer, in ARM
 * instructions, on this device's own ISA?
 *
 * This is the graphics half of the 60fps question, and unlike every earlier
 * cps1 rig it is not measuring a made-up scene. The inputs are a byte-exact
 * capture of the game's own state -- gfxram and the CPS-A registers, taken
 * from linux/cps1/real_frame_harness.c after 3600 booted frames, at the
 * point where it renders the title screen -- plus the real GFX ROM. The rig
 * rebuilds tilemaps, palette and scroll from those exactly as the harness
 * does, then times the SHIPPING renderer (cps1_bg.c / cps1_ppu.c).
 *
 * Pair it with rig_cps1_m68k.c, which bounds the CPU half at 18.3% of the
 * frame budget, and the two together answer whether 60fps is reachable.
 *
 * The usual caveat still applies and is not a formality here: QEMU models
 * neither cache misses nor flash wait states, and this renderer walks a
 * 4 MB GFX ROM that on the device is XIP out of external flash. The real
 * part will cost MORE than this. Treat it as a floor.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "cps1_rom.h"
#include "cps1_ppu.h"
#include "cps1_bg.h"

#ifndef RIG_FRAMES
#define RIG_FRAMES 20
#endif

#define DEVICE_CLOCK_HZ    340000000ull
#define DEVICE_BUDGET_INSN ((uint64_t)(DEVICE_CLOCK_HZ / 60ull))
#define GFXRAM_BYTES       0x30000u

void rig_timer_init(void);
uint32_t rig_timer_now(void);
uint32_t rig_calibrate(uint32_t n);

/* Blobs linked in via objcopy -I binary (see run_cps1_frame.sh). */
extern const uint8_t _binary_gfxram_bin_start[];
extern const uint8_t _binary_cpsregs_bin_start[];
extern const uint8_t _binary_gfxrom_bin_start[];
extern const uint8_t _binary_gfxrom_bin_end[];

static cps1_rom_t        s_rom;
static cps1_tile_cache_t s_cache;
static cps1_palette_t    s_pal;
static cps1_bg_state_t   s_bg;
static cps1_oam_t        s_oam;
static uint16_t s_fb[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
static uint8_t  s_meta[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];

static const uint8_t  *g_gfxram;
static const uint16_t *g_regs;

#define CPSA_REG(i) g_regs[(0x100u + (i) * 2u) >> 1]
enum { REG_OBJ = 0, REG_SCROLL1, REG_SCROLL2, REG_SCROLL3, REG_OTHER, REG_PALETTE };

static uint32_t resolve_base(uint16_t reg)
{
    uint32_t b = (uint32_t)reg * 256u;
    b &= ~(0x4000u - 1u);
    return b % GFXRAM_BYTES;
}

static uint16_t gfx_word(uint32_t off)
{
    off %= GFXRAM_BYTES;
    return (uint16_t)((g_gfxram[off] << 8) | g_gfxram[off + 1]);
}

static const unsigned draw_order[CPS1_BG_LAYER_COUNT] = {
    CPS1_BG_SCROLL3, CPS1_BG_SCROLL2, CPS1_BG_SCROLL1
};

/* One frame of rendering, back to front, exactly as the harness does it.
 *
 * `meta` is the parameter that decides WHICH PROGRAM this rig is. The device
 * calls cps1_render_into(fb, NULL) -- main_cps1.c:794 -- so on hardware the
 * blitter writes colour only. The original measurement here passed s_meta and
 * therefore also paid for a per-pixel priority stamp the device never writes.
 * Both are reported below; the device-shaped one is the one to optimise
 * against. (Root CLAUDE.md: a harness that is a different program proves
 * nothing -- that rule does not stop applying when the difference makes the
 * harness look SLOWER than the device.) */
static void render_frame_meta(uint8_t *meta)
{
    memset(s_fb, 0, sizeof(s_fb));
    if (meta)
        memset(meta, 0, (size_t)CPS1_FB_WIDTH * CPS1_FB_HEIGHT);
    for (unsigned i = 0; i < CPS1_BG_LAYER_COUNT; i++) {
        unsigned L = draw_order[i];
        cps1_bg_render_layer(&s_bg.layers[L], L, &s_rom, &s_cache, &s_pal, s_fb, meta);
    }
    cps1_ppu_render(&s_oam, &s_rom, &s_cache, &s_pal, s_fb);
}

static void render_frame(void) { render_frame_meta(s_meta); }

/*
 * The same frame, with the overdraw skipped.
 *
 * Coverage is built top-down over the layers that sit ABOVE the one being
 * drawn, then the draw runs in the usual back-to-front order and declines to
 * blit any 8x8 sub-tile whose footprint is already fully covered. Nothing
 * about the surviving pixels changes -- which is the claim the byte-for-byte
 * framebuffer compare in main() exists to hold to.
 *
 * SCROLL1 is drawn with no skip mask: nothing is above it but sprites, and
 * the measurement says it loses 0% to overdraw, so a coverage pass for it
 * would be pure cost. Sprites are likewise left alone.
 */
static cps1_cover_t s_cov_above_s2;   /* SCROLL1                */
static cps1_cover_t s_cov_above_s3;   /* SCROLL1 + SCROLL2      */

static void render_frame_covered(void)
{
    memset(s_fb, 0, sizeof(s_fb));

    cps1_cover_reset(&s_cov_above_s2);
    cps1_bg_render_layer_ex(&s_bg.layers[CPS1_BG_SCROLL1], CPS1_BG_SCROLL1, &s_rom,
                             &s_cache, &s_pal, NULL, NULL, NULL, &s_cov_above_s2);
    s_cov_above_s3 = s_cov_above_s2;
    cps1_bg_render_layer_ex(&s_bg.layers[CPS1_BG_SCROLL2], CPS1_BG_SCROLL2, &s_rom,
                             &s_cache, &s_pal, NULL, NULL, NULL, &s_cov_above_s3);

    cps1_bg_render_layer_ex(&s_bg.layers[CPS1_BG_SCROLL3], CPS1_BG_SCROLL3, &s_rom,
                             &s_cache, &s_pal, s_fb, NULL, &s_cov_above_s3, NULL);
    cps1_bg_render_layer_ex(&s_bg.layers[CPS1_BG_SCROLL2], CPS1_BG_SCROLL2, &s_rom,
                             &s_cache, &s_pal, s_fb, NULL, &s_cov_above_s2, NULL);
    cps1_bg_render_layer_ex(&s_bg.layers[CPS1_BG_SCROLL1], CPS1_BG_SCROLL1, &s_rom,
                             &s_cache, &s_pal, s_fb, NULL, NULL, NULL);
    cps1_ppu_render(&s_oam, &s_rom, &s_cache, &s_pal, s_fb);
}

int main(void)
{
    rig_timer_init();
    uint32_t cal = rig_calibrate(1000000);
    uint32_t ipt_x1000 = (uint32_t)((3000000ull * 1000ull) / (cal ? cal : 1));

    g_gfxram = _binary_gfxram_bin_start;
    g_regs   = (const uint16_t *)(const void *)_binary_cpsregs_bin_start;

    cps1_rom_region_t gfx = {
        _binary_gfxrom_bin_start,
        (uint32_t)(_binary_gfxrom_bin_end - _binary_gfxrom_bin_start)
    };
    /* prg is required by cps1_rom_attach but unused by the renderer; point it
     * at the gfx blob's head so the validity check passes without carrying a
     * second megabyte of payload the measurement does not need. */
    cps1_rom_region_t prg = { _binary_gfxrom_bin_start, 0x1000u };
    cps1_rom_region_t none = { 0, 0 };
    if (cps1_rom_attach(&s_rom, prg, gfx, none, none) != 0) {
        printf("[cps1-frame] rom attach failed\n");
        return 1;
    }

    cps1_tile_cache_reset(&s_cache);
    s_cache.real_gfx = 1;

    uint32_t pal_base = resolve_base(CPSA_REG(REG_PALETTE));
    for (unsigned b = 0; b < CPS1_PALETTE_BANKS; b++)
        for (unsigned c = 0; c < CPS1_PALETTE_COLORS; c++)
            s_pal.colors[b][c] =
                cps1_palette_build(gfx_word(pal_base + (b * CPS1_PALETTE_COLORS + c) * 2u));

    cps1_bg_reset(&s_bg);
    for (unsigned L = 0; L < CPS1_BG_LAYER_COUNT; L++) {
        uint32_t base = resolve_base(CPSA_REG(REG_SCROLL1 + L));
        for (unsigned sc = 0; sc < CPS1_BG_MAP_W * CPS1_BG_MAP_H; sc++) {
            unsigned col, row;
            cps1_bg_swizzle_offset_to_col_row(L, sc, &col, &row);
            cps1_bg_cell_t *cell = &s_bg.layers[L].cells[row * CPS1_BG_MAP_W + col];
            cell->code = gfx_word(base + sc * 4u);
            cell->attr = gfx_word(base + sc * 4u + 2u);
        }
    }
    s_bg.layers[CPS1_BG_SCROLL1].scroll_x = (int16_t)CPSA_REG(6);
    s_bg.layers[CPS1_BG_SCROLL1].scroll_y = (int16_t)CPSA_REG(7);
    s_bg.layers[CPS1_BG_SCROLL2].scroll_x = (int16_t)CPSA_REG(8);
    s_bg.layers[CPS1_BG_SCROLL2].scroll_y = (int16_t)CPSA_REG(9);
    s_bg.layers[CPS1_BG_SCROLL3].scroll_x = (int16_t)CPSA_REG(10);
    s_bg.layers[CPS1_BG_SCROLL3].scroll_y = (int16_t)CPSA_REG(11);

    uint32_t obj_base = resolve_base(CPSA_REG(REG_OBJ));
    s_oam.count = 0;
    for (unsigned i = 0; i < CPS1_OAM_MAX_SPRITES; i++) {
        uint16_t x = gfx_word(obj_base + i * 8u), y = gfx_word(obj_base + i * 8u + 2u);
        if (x == 0xFFFF && y == 0xFFFF) break;
        cps1_oam_entry_t *s = &s_oam.sprites[s_oam.count++];
        s->x = (int16_t)x; s->y = (int16_t)y;
        s->tile_index = gfx_word(obj_base + i * 8u + 4u);
        s->attr = gfx_word(obj_base + i * 8u + 6u);
        s->enabled = 1;
    }

    printf("[cps1-frame] real title-screen state: OBJ=%u sprites, scroll S1=(%d,%d) "
           "S2=(%d,%d) S3=(%d,%d)\n", s_oam.count,
           s_bg.layers[0].scroll_x, s_bg.layers[0].scroll_y,
           s_bg.layers[1].scroll_x, s_bg.layers[1].scroll_y,
           s_bg.layers[2].scroll_x, s_bg.layers[2].scroll_y);

    render_frame();   /* warm the tile cache, as a steady-state frame would be */

    uint32_t t0 = rig_timer_now();
    for (unsigned f = 0; f < RIG_FRAMES; f++)
        render_frame();
    uint32_t t1 = rig_timer_now();

    uint64_t insns = (uint64_t)(uint32_t)(t1 - t0) * ipt_x1000 / 1000ull / RIG_FRAMES;
    double ms = (double)insns * 1000.0 / (double)DEVICE_CLOCK_HZ;
    uint64_t pct_x10 = insns * 1000ull / DEVICE_BUDGET_INSN;

    unsigned lit = 0;
    for (unsigned i = 0; i < CPS1_FB_WIDTH * CPS1_FB_HEIGHT; i++) if (s_fb[i]) lit++;

    printf("[cps1-frame] framebuffer %u/%u lit, tile cache %u hits / %u misses\n",
           lit, CPS1_FB_WIDTH * CPS1_FB_HEIGHT, s_cache.hits, s_cache.misses);
    printf("[cps1-frame] GRAPHICS: %lu insn/frame = %.3f ms @340MHz = %lu.%lu%% of budget\n",
           (unsigned long)insns, ms, (unsigned long)(pct_x10 / 10), (unsigned long)(pct_x10 % 10));
    printf("[cps1-frame] CPU half was 1,039,495 insn (18.3%%); TOTAL = %lu insn/frame "
           "= %.3f ms vs 16.667 ms budget -> %s\n",
           (unsigned long)(insns + 1039495ull),
           (double)(insns + 1039495ull) * 1000.0 / (double)DEVICE_CLOCK_HZ,
           (insns + 1039495ull <= DEVICE_BUDGET_INSN) ? "UNDER" : "OVER");
    printf("[cps1-frame] NOTE: floor only -- QEMU models no cache misses and no flash "
           "wait states, and the 4MB GFX ROM is XIP on the real part.\n");

    /* ---- what the DEVICE actually runs: cps1_render_into(fb, NULL) ---- */
    render_frame_meta(NULL);
    t0 = rig_timer_now();
    for (unsigned f = 0; f < RIG_FRAMES; f++)
        render_frame_meta(NULL);
    t1 = rig_timer_now();
    uint64_t dev = (uint64_t)(uint32_t)(t1 - t0) * ipt_x1000 / 1000ull / RIG_FRAMES;
    printf("[cps1-frame] DEVICE SHAPE (meta=NULL, as main_cps1.c:794 calls it): "
           "%lu insn/frame = %.3f ms = %lu.%lu%% of budget  [meta stamp costs %lu insn/frame, "
           "%lu.%lu%% of the measured graphics cost]\n",
           (unsigned long)dev, (double)dev * 1000.0 / (double)DEVICE_CLOCK_HZ,
           (unsigned long)(dev * 1000ull / DEVICE_BUDGET_INSN / 10),
           (unsigned long)(dev * 1000ull / DEVICE_BUDGET_INSN % 10),
           (unsigned long)(insns - dev),
           (unsigned long)((insns - dev) * 1000ull / insns / 10),
           (unsigned long)((insns - dev) * 1000ull / insns % 10));

    /* ---- where the device-shaped cost goes: per stage ---- */
    static const char *stage_name[CPS1_BG_LAYER_COUNT] = { "SCROLL3", "SCROLL2", "SCROLL1" };
    uint64_t stage_total = 0;
    for (unsigned i = 0; i < CPS1_BG_LAYER_COUNT; i++) {
        unsigned L = draw_order[i];
        memset(s_fb, 0, sizeof(s_fb));
        cps1_bg_render_layer(&s_bg.layers[L], L, &s_rom, &s_cache, &s_pal, s_fb, NULL);
        t0 = rig_timer_now();
        for (unsigned f = 0; f < RIG_FRAMES; f++)
            cps1_bg_render_layer(&s_bg.layers[L], L, &s_rom, &s_cache, &s_pal, s_fb, NULL);
        t1 = rig_timer_now();
        uint64_t s = (uint64_t)(uint32_t)(t1 - t0) * ipt_x1000 / 1000ull / RIG_FRAMES;
        stage_total += s;
        printf("[cps1-frame]   stage %-8s %8lu insn/frame  %lu.%lu%% of device shape\n",
               stage_name[i], (unsigned long)s,
               (unsigned long)(s * 1000ull / dev / 10), (unsigned long)(s * 1000ull / dev % 10));
    }
    cps1_ppu_render(&s_oam, &s_rom, &s_cache, &s_pal, s_fb);
    t0 = rig_timer_now();
    for (unsigned f = 0; f < RIG_FRAMES; f++)
        cps1_ppu_render(&s_oam, &s_rom, &s_cache, &s_pal, s_fb);
    t1 = rig_timer_now();
    uint64_t spr = (uint64_t)(uint32_t)(t1 - t0) * ipt_x1000 / 1000ull / RIG_FRAMES;
    stage_total += spr;
    printf("[cps1-frame]   stage %-8s %8lu insn/frame  %lu.%lu%% of device shape\n",
           "SPRITES", (unsigned long)spr,
           (unsigned long)(spr * 1000ull / dev / 10), (unsigned long)(spr * 1000ull / dev % 10));
    printf("[cps1-frame]   stages sum %lu vs device-shape frame %lu (difference = the "
           "per-frame framebuffer clear, which the stage runs do not repeat)\n",
           (unsigned long)stage_total, (unsigned long)dev);

    /* ---- overdraw: how much of each layer survives to the visible frame? ----
     *
     * The three BG layers cost 26% each, dead flat, which is the signature of
     * three full-screen blits stacked back to front. What matters for
     * optimisation is not what a layer DRAWS but what of it is still visible
     * once the layers above it have drawn: work whose only effect is to be
     * overwritten can be skipped at tile granularity without changing a pixel.
     *
     * The opacity mask is the meta buffer, not "framebuffer pixel != 0" -- a
     * legitimately black pen would make that lie. meta is stamped if and only
     * if the source pen was non-transparent. */
    static uint8_t s_mask[CPS1_BG_LAYER_COUNT][CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
    for (unsigned i = 0; i < CPS1_BG_LAYER_COUNT; i++) {
        unsigned L = draw_order[i];
        memset(s_fb, 0, sizeof(s_fb));
        memset(s_mask[i], 0, sizeof(s_mask[i]));
        cps1_bg_render_layer(&s_bg.layers[L], L, &s_rom, &s_cache, &s_pal, s_fb, s_mask[i]);
    }
    printf("[cps1-frame] overdraw (back to front; 'survives' = not covered by any layer above):\n");
    for (unsigned i = 0; i < CPS1_BG_LAYER_COUNT; i++) {
        unsigned drawn = 0, survives = 0;
        for (unsigned p = 0; p < CPS1_FB_WIDTH * CPS1_FB_HEIGHT; p++) {
            if (!s_mask[i][p])
                continue;
            drawn++;
            unsigned covered = 0;
            for (unsigned j = i + 1; j < CPS1_BG_LAYER_COUNT; j++)
                if (s_mask[j][p]) { covered = 1; break; }
            if (!covered)
                survives++;
        }
        printf("[cps1-frame]   %-8s draws %6u px, %6u survive (%2u%%) -- %6u px of work "
               "(%2u%) is overwritten\n", stage_name[i], drawn, survives,
               drawn ? survives * 100u / drawn : 0, drawn - survives,
               drawn ? (drawn - survives) * 100u / drawn : 0);
    }

    /* ---- skipping that overdraw: same pixels, fewer of them written ----
     * The gate first. A faster renderer that draws a different frame is not a
     * faster renderer, and "looks right" is not a check -- so hash both
     * framebuffers and refuse to report a speedup unless they are identical. */
    static uint16_t s_ref[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
    render_frame_meta(NULL);
    memcpy(s_ref, s_fb, sizeof(s_ref));
    render_frame_covered();
    unsigned diff = 0, first = 0;
    for (unsigned i = 0; i < CPS1_FB_WIDTH * CPS1_FB_HEIGHT; i++) {
        if (s_ref[i] != s_fb[i]) {
            if (!diff) first = i;
            diff++;
        }
    }
    if (diff) {
        printf("[cps1-frame] FAIL coverage-skip changed %u pixels (first at %u,%u: "
               "%04x vs %04x) -- NOT a speedup, a bug\n", diff,
               first % CPS1_FB_WIDTH, first / CPS1_FB_WIDTH, s_ref[first], s_fb[first]);
        return 1;
    }
    printf("[cps1-frame] coverage-skip output is byte-identical to the reference frame\n");

    t0 = rig_timer_now();
    for (unsigned f = 0; f < RIG_FRAMES; f++)
        render_frame_covered();
    t1 = rig_timer_now();
    uint64_t cov = (uint64_t)(uint32_t)(t1 - t0) * ipt_x1000 / 1000ull / RIG_FRAMES;
    printf("[cps1-frame] COVERAGE-SKIP: %lu insn/frame = %.3f ms = %lu.%lu%% of budget "
           "(was %lu = %lu.%lu%%; graphics -%lu.%lu%%)\n",
           (unsigned long)cov, (double)cov * 1000.0 / (double)DEVICE_CLOCK_HZ,
           (unsigned long)(cov * 1000ull / DEVICE_BUDGET_INSN / 10),
           (unsigned long)(cov * 1000ull / DEVICE_BUDGET_INSN % 10),
           (unsigned long)dev,
           (unsigned long)(dev * 1000ull / DEVICE_BUDGET_INSN / 10),
           (unsigned long)(dev * 1000ull / DEVICE_BUDGET_INSN % 10),
           (unsigned long)((dev - cov) * 1000ull / dev / 10),
           (unsigned long)((dev - cov) * 1000ull / dev % 10));
    /* What the two coverage passes cost on their own. This is the price paid
     * in a scene with NOTHING to skip -- the honest worst case, and the number
     * that decides whether the mask is safe to leave on unconditionally. */
    t0 = rig_timer_now();
    for (unsigned f = 0; f < RIG_FRAMES; f++) {
        cps1_cover_reset(&s_cov_above_s2);
        cps1_bg_render_layer_ex(&s_bg.layers[CPS1_BG_SCROLL1], CPS1_BG_SCROLL1, &s_rom,
                                 &s_cache, &s_pal, NULL, NULL, NULL, &s_cov_above_s2);
        s_cov_above_s3 = s_cov_above_s2;
        cps1_bg_render_layer_ex(&s_bg.layers[CPS1_BG_SCROLL2], CPS1_BG_SCROLL2, &s_rom,
                                 &s_cache, &s_pal, NULL, NULL, NULL, &s_cov_above_s3);
    }
    t1 = rig_timer_now();
    uint64_t cpass = (uint64_t)(uint32_t)(t1 - t0) * ipt_x1000 / 1000ull / RIG_FRAMES;
    printf("[cps1-frame] coverage passes alone: %lu insn/frame = %lu.%lu%% of the "
           "unskipped frame -- the worst case, i.e. what a scene with no overdraw pays\n",
           (unsigned long)cpass,
           (unsigned long)(cpass * 1000ull / dev / 10), (unsigned long)(cpass * 1000ull / dev % 10));
    printf("[cps1-frame] FRAME TOTAL: %lu insn = %.3f ms vs 16.667 ms -> %s\n",
           (unsigned long)(cov + 1039495ull),
           (double)(cov + 1039495ull) * 1000.0 / (double)DEVICE_CLOCK_HZ,
           (cov + 1039495ull <= DEVICE_BUDGET_INSN) ? "UNDER" : "OVER");
    return 0;
}
