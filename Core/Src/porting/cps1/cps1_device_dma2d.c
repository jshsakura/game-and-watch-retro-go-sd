/*
 * Device-only DMA2D (Chrom-ART) offload for cps1's tile blitter (docs/
 * CPS1_MAME_ALIGNMENT.md section 9, optimization phase). Two things this
 * file does, both real, working DMA2D usage patterns from this exact
 * codebase's own Core/Src/porting/lib/hw_jpeg_decoder.c (DMA2D_Handle,
 * HAL_DMA2D_Init/ConfigLayer/Start/PollForTransfer -- NOT invented from
 * scratch the way LTDC layer 1 had to be):
 *
 *   1) cps1_device_dma2d_clear_rgb565(): DMA2D_R2M (register-to-memory
 *      fill) -- offloads the plain memset()-style framebuffer clears
 *      cps1_core.c currently does on the CPU (cps1_core_run_frame*'s
 *      memset calls) onto the DMA2D peripheral, freeing the CPU to keep
 *      running while the fill happens.
 *
 *   2) cps1_device_dma2d_load_clut() + cps1_device_dma2d_blit_tile_indexed():
 *      the actual tile-decode-and-blit offload -- DMA2D's L4 (4bpp
 *      indexed) input format converts cps1's packed 4bpp tile data
 *      directly to RGB565 output pixels IN HARDWARE via a loaded CLUT
 *      (color lookup table), replacing what cps1_blit8x8_indexed's
 *      per-pixel nibble-unpack + palette-lookup loop does on the CPU.
 *      DMA2D_M2M_BLEND mode (foreground = indexed tile via CLUT,
 *      background = the framebuffer's OWN existing content at the
 *      destination, output = same destination) makes CLUT entry 0's
 *      alpha=0 punch through to the existing background pixel --
 *      hardware transparency, matching cps1's "pen index 0 = transparent"
 *      convention everywhere, with no CPU-side branch per pixel.
 *
 * ================================ HONESTY NOTE ================================
 * Unlike cps1_device_display.c's LTDC layer 1 (zero in-repo precedent),
 * DMA2D itself is a PROVEN, shipped feature of this codebase (hw_jpeg_
 * decoder.c uses DMA2D_M2M_BLEND_BG for JPEG-to-framebuffer compositing
 * today) -- the Init/ConfigLayer/Start/PollForTransfer call SHAPE here is
 * that same, working pattern, not a first-of-its-kind guess. What is
 * NOT re-derivable from that precedent, and remains UNVERIFIED without
 * real hardware or the STM32H7 reference manual (RM0433) in hand:
 *   - The L4 input format's nibble-to-pixel-index mapping. This file
 *     assumes pixel 0 of a byte pair is the LOW nibble (ST's documented
 *     convention per RM0433's DMA2D chapter, to the best of this
 *     project's current knowledge) -- OPPOSITE of cps1_rom_decode_tile_
 *     planar's own packed-4bpp output (high nibble = first/even pixel,
 *     docs/CPS1_MAME_ALIGNMENT.md section 1). cps1_device_dma2d_blit_
 *     tile_indexed therefore nibble-swaps its OWN copy of the tile data
 *     before handing it to DMA2D (see cps1_nibble_swap_tile below) --
 *     if this assumption is backwards, the real-hardware symptom is
 *     every ADJACENT PIXEL PAIR swapped (a recognizable, specific
 *     artifact to check for, not a crash), and the fix is deleting the
 *     swap rather than guessing further.
 *   - Whether HAL_DMA2D_PollForTransfer alone is sufficient after
 *     HAL_DMA2D_CLUTLoad, or whether a game with per-frame palette
 *     changes needs the CLUT reloaded (and awaited) more precisely than
 *     this call-per-blit-call structure currently does.
 * What IS verified: this file compiles against the real STM32H7xx HAL
 * headers (DMA2D_HandleTypeDef, DMA2D_CLUTCfgTypeDef, HAL_DMA2D_*) with
 * the project's real arm-none-eabi-gcc toolchain -- see the optimization-
 * phase commit message for the exact command and result.
 * ================================================================================
 */
#include "stm32h7xx_hal.h"
#include "cps1_core.h"
#include "cps1_ppu.h"

static DMA2D_HandleTypeDef s_dma2d;

/* --- 1) Framebuffer clear via DMA2D_R2M (register-to-memory fill) --- */

void cps1_device_dma2d_clear_rgb565(uint16_t *fb, uint32_t width, uint32_t height, uint16_t color)
{
    s_dma2d.Instance = DMA2D;
    s_dma2d.Init.Mode = DMA2D_R2M;
    s_dma2d.Init.ColorMode = DMA2D_OUTPUT_RGB565;
    s_dma2d.Init.OutputOffset = 0;
    s_dma2d.Init.AlphaInverted = DMA2D_REGULAR_ALPHA;
    s_dma2d.Init.RedBlueSwap = DMA2D_RB_REGULAR;
    s_dma2d.Init.LineOffsetMode = DMA2D_LOM_PIXELS;

    HAL_DMA2D_Init(&s_dma2d);
    HAL_DMA2D_Start(&s_dma2d, color, (uint32_t)fb, width, height);
    HAL_DMA2D_PollForTransfer(&s_dma2d, 50);
}

/* --- 2) Indexed-tile hardware blit via DMA2D L4 input + CLUT + blend --- */

/* ARGB8888 CLUT, 16 entries (4bpp) -- index 0 alpha=0 (transparent, punches
 * through to the existing background in DMA2D_M2M_BLEND mode), 1-15
 * alpha=0xFF (opaque). Rebuilt from a cps1_palette_t bank on demand
 * (cps1_palette_t stores RGB565; DMA2D's CLUT wants ARGB8888). */
static uint32_t s_clut_argb8888[16];
static DMA2D_CLUTCfgTypeDef s_clut_cfg;

static uint32_t cps1_rgb565_to_argb8888(uint16_t rgb565, uint8_t alpha)
{
    unsigned r5 = (rgb565 >> 11) & 0x1Fu, g6 = (rgb565 >> 5) & 0x3Fu, b5 = rgb565 & 0x1Fu;
    unsigned r8 = (r5 << 3) | (r5 >> 2);
    unsigned g8 = (g6 << 2) | (g6 >> 4);
    unsigned b8 = (b5 << 3) | (b5 >> 2);
    return ((uint32_t)alpha << 24) | (r8 << 16) | (g8 << 8) | b8;
}

/* Loads bank's 16 colors into the foreground layer's CLUT and waits for
 * the load to complete before returning -- must be called (again, if the
 * palette bank changed) before cps1_device_dma2d_blit_tile_indexed uses
 * that bank, since DMA2D reads through whatever CLUT is currently loaded,
 * not a fresh one per blit call. */
void cps1_device_dma2d_load_clut(const cps1_palette_t *pal, unsigned bank)
{
    unsigned b = bank & (CPS1_PALETTE_BANKS - 1u);
    s_clut_argb8888[0] = cps1_rgb565_to_argb8888(pal->colors[b][0], 0x00u); /* pen 0: transparent */
    for (unsigned i = 1; i < 16; i++)
        s_clut_argb8888[i] = cps1_rgb565_to_argb8888(pal->colors[b][i], 0xFFu);

    s_dma2d.Instance = DMA2D;
    s_dma2d.Init.Mode = DMA2D_M2M_BLEND;
    s_dma2d.Init.ColorMode = DMA2D_OUTPUT_RGB565;
    s_dma2d.Init.OutputOffset = 0;
    s_dma2d.Init.AlphaInverted = DMA2D_REGULAR_ALPHA;
    s_dma2d.Init.RedBlueSwap = DMA2D_RB_REGULAR;
    s_dma2d.Init.LineOffsetMode = DMA2D_LOM_PIXELS;
    HAL_DMA2D_Init(&s_dma2d);

    s_dma2d.LayerCfg[1].InputColorMode = DMA2D_INPUT_L4;
    s_dma2d.LayerCfg[1].AlphaMode = DMA2D_NO_MODIF_ALPHA; /* use the CLUT's own per-entry alpha */
    s_dma2d.LayerCfg[1].InputAlpha = 0xFF;
    s_dma2d.LayerCfg[1].InputOffset = 0;
    s_dma2d.LayerCfg[1].RedBlueSwap = DMA2D_RB_REGULAR;
    s_dma2d.LayerCfg[1].AlphaInverted = DMA2D_REGULAR_ALPHA;
    HAL_DMA2D_ConfigLayer(&s_dma2d, 1);

    s_clut_cfg.CLUTColorMode = DMA2D_CCM_ARGB8888;
    s_clut_cfg.Size = 15; /* 0-based: 16 entries */
    s_clut_cfg.pCLUT = s_clut_argb8888;
    HAL_DMA2D_CLUTLoad(&s_dma2d, s_clut_cfg, 1);
    HAL_DMA2D_PollForTransfer(&s_dma2d, 50);
}

/* Nibble-swaps a copy of an 8x8 packed-4bpp tile (32 bytes) so DMA2D's L4
 * reader (pixel 0 = low nibble, per this file's HONESTY NOTE) sees the
 * SAME pixel order cps1_rom_decode_tile_planar produced (pixel 0 = high
 * nibble). A 32-byte stack scratch buffer, not an in-place swap of the
 * cache's own copy -- the tile cache's stored bytes must stay in cps1's
 * own convention for the CPU blit path (cps1_blit8x8_indexed) to keep
 * working unchanged. */
static void cps1_nibble_swap_tile(const uint8_t *src, uint8_t *dst_swapped)
{
    for (int i = 0; i < CPS1_TILE_SIZE_BYTES; i++)
        dst_swapped[i] = (uint8_t)((src[i] << 4) | (src[i] >> 4));
}

/* Blits one already-cached 8x8 packed-4bpp tile straight from indexed
 * data to RGB565 output, blended over dst_fb's EXISTING content at
 * (dst_x,dst_y) so pen-0 pixels (CLUT alpha=0) show the background
 * through, entirely in DMA2D hardware -- no CPU nibble-unpack, no
 * per-pixel palette lookup, no per-pixel branch. Caller must have called
 * cps1_device_dma2d_load_clut() for `bank` first. dst_stride is the
 * framebuffer's row width in pixels (CPS1_FB_WIDTH for cps1's own
 * buffers). Does NOT clip to screen bounds -- caller's responsibility,
 * same contract cps1_blit8x8_indexed's fast path already establishes. */
void cps1_device_dma2d_blit_tile_indexed(const uint8_t *tile4bpp, uint16_t *dst_fb,
                                          int dst_x, int dst_y, uint32_t dst_stride)
{
    uint8_t swapped[CPS1_TILE_SIZE_BYTES];
    cps1_nibble_swap_tile(tile4bpp, swapped);

    uint32_t dst_addr = (uint32_t)(dst_fb + (uint32_t)dst_y * dst_stride + (uint32_t)dst_x);

    s_dma2d.Init.OutputOffset = dst_stride - 8u;
    HAL_DMA2D_Init(&s_dma2d);
    s_dma2d.LayerCfg[1].InputOffset = 0; /* tile is tightly packed, 8px/row, no padding */
    HAL_DMA2D_ConfigLayer(&s_dma2d, 1);

    /* Background layer (0): read the framebuffer's OWN existing content
     * at the destination so blend-mode's alpha compositing has something
     * to punch through TO -- same stride as the output, no format
     * conversion needed (already RGB565). */
    s_dma2d.LayerCfg[0].InputColorMode = DMA2D_INPUT_RGB565;
    s_dma2d.LayerCfg[0].AlphaMode = DMA2D_NO_MODIF_ALPHA;
    s_dma2d.LayerCfg[0].InputAlpha = 0xFF;
    s_dma2d.LayerCfg[0].InputOffset = dst_stride - 8u;
    s_dma2d.LayerCfg[0].RedBlueSwap = DMA2D_RB_REGULAR;
    s_dma2d.LayerCfg[0].AlphaInverted = DMA2D_REGULAR_ALPHA;
    HAL_DMA2D_ConfigLayer(&s_dma2d, 0);

    HAL_DMA2D_BlendingStart(&s_dma2d, (uint32_t)swapped, dst_addr, dst_addr, 8, 8);
    HAL_DMA2D_PollForTransfer(&s_dma2d, 50);
}
