/*
 * Device-only LTDC dual-layer binding (docs/CPS1_MAME_ALIGNMENT.md
 * section 9, cheat 8 / docs/CPS1_SENIOR_TRICKS_ANALYSIS.md [치트 8]):
 * SCROLL1+SCROLL2 (cps1_core_render_ltdc_bottom(), Phase 12) become LTDC
 * hardware layer 0; SCROLL3+sprites (cps1_core_run_frame_device_cost(),
 * unchanged since Phase 7/10) become layer 1. LTDC's own scanout hardware
 * blends the two -- the thing cheat 8 actually saves is that per-pixel
 * blend/compositing pass, NOT the BG tile rendering itself (both layers'
 * CONTENT is still real CPU work; see cps1_core.h's comments on both
 * functions).
 *
 * ================================ HONESTY NOTE ================================
 * This is genuinely new territory for this codebase: grep confirms EVERY
 * existing HAL_LTDC_ConfigLayer/SetAddress/ConfigCLUT call across every
 * other core and Core/Src/gw_lcd.c passes LayerIdx 0 -- LTDC's second
 * hardware layer (index 1) has never been configured or used by ANY core
 * in this repository before this file. There is no in-repo precedent to
 * follow, no prior working example to copy, and no way to verify actual
 * scanout/blending behavior without physical hardware (which this project
 * does not have) -- the QEMU M7 rig models CPU instruction counts only,
 * not the LTDC peripheral. What IS verified: this file compiles against
 * the real STM32H7xx HAL headers and the real hltdc/LTDC_LayerCfgTypeDef
 * types with the project's actual toolchain (see the Phase 12 commit
 * message for the exact command and result) -- a real, if partial,
 * correctness gate (catches type/API-shape errors), not a substitute for
 * flashing real hardware with a real ROM and looking at the screen.
 * ================================================================================
 *
 * Panel/resolution mismatch (also new, also unaddressed by any prior
 * phase): GW_LCD_WIDTH/HEIGHT (gw_lcd.h) is 320x240; CPS-1's native
 * resolution is 384x224. Height fits (8px letterbox); width does not --
 * cps1_core_crop_to_panel() center-crops to CPS1_PANEL_WIDTH (320) before
 * either buffer ever reaches LTDC, so both layers use the SAME simple,
 * standard-HAL-only per-layer pitch as every other core (ImageWidth ==
 * WindowX1-WindowX0 == 320, no manual LTDC_LxCFBLR pitch override needed).
 * This trades a small per-frame CPU copy (2 * 320*224 pixels) for
 * avoiding an unverifiable manual register poke -- see cps1_core.c's
 * comment on cps1_core_crop_to_panel for why "crop" (not scale) is a
 * placeholder choice, not a validated design decision.
 */
#include "stm32h7xx_hal.h"
#include "gw_lcd.h"
#include "cps1_core.h"

extern LTDC_HandleTypeDef hltdc;

#define CPS1_LTDC_LAYER_BOTTOM 0u /* SCROLL1+SCROLL2 -- docs call this "Layer 1" (1-based) */
#define CPS1_LTDC_LAYER_TOP    1u /* SCROLL3+sprites -- docs call this "Layer 2" (1-based) */

/* Transparent-pixel convention every cps1_*.c render path already uses
 * (index 0 = transparent, never written) -- LTDC's color-keying feature
 * makes THIS SAME convention work for hardware blending: any top-layer
 * pixel still holding the RGB565 value 0x0000 (i.e. never drawn over by
 * SCROLL3/sprite content) is treated as transparent by LTDC itself, no
 * CPU-side alpha channel needed. */
#define CPS1_LTDC_COLOR_KEY_RGB565 0x0000u

/* Panel-cropped copies LTDC actually scans out -- see file header. Not
 * yet placed in a specific RAM_EMU/overlay section: cps1 has no real
 * linked .overlay_cps1 yet (docs section 9 Phase 11/12 both predate that
 * integration step), so these currently just land wherever this
 * translation unit's .bss ends up. Revisit placement (likely AHB/overlay
 * BSS, matching how other cores keep large device-only buffers out of
 * the shared DTCM heap) once cps1 is wired into the real system list. */
static uint16_t s_panel_bottom[CPS1_PANEL_WIDTH * CPS1_PANEL_HEIGHT];
static uint16_t s_panel_top[CPS1_PANEL_WIDTH * CPS1_PANEL_HEIGHT];

static void cps1_device_display_config_layer(uint32_t layer_idx, uint16_t *fb)
{
    LTDC_LayerCfgTypeDef cfg = {0};
    cfg.WindowX0 = 0;
    cfg.WindowX1 = CPS1_PANEL_WIDTH;
    cfg.WindowY0 = (GW_LCD_HEIGHT - CPS1_PANEL_HEIGHT) / 2; /* 8px letterbox, see file header */
    cfg.WindowY1 = cfg.WindowY0 + CPS1_PANEL_HEIGHT;
    cfg.PixelFormat = LTDC_PIXEL_FORMAT_RGB565;
    cfg.Alpha = 255;
    cfg.Alpha0 = 0;
    cfg.BlendingFactor1 = LTDC_BLENDING_FACTOR1_CA;
    cfg.BlendingFactor2 = LTDC_BLENDING_FACTOR2_CA;
    cfg.FBStartAdress = (uint32_t)fb;
    cfg.ImageWidth = CPS1_PANEL_WIDTH;
    cfg.ImageHeight = CPS1_PANEL_HEIGHT;
    cfg.Backcolor.Red = 0;
    cfg.Backcolor.Green = 0;
    cfg.Backcolor.Blue = 0;

    HAL_LTDC_ConfigLayer(&hltdc, &cfg, layer_idx);
}

/* Call once at CPS1 boot (after odroid_system_init/lcd_init, matching
 * every other core's own display-setup sequence): configures both LTDC
 * layers and enables color keying on the top layer so its transparent
 * (0x0000) pixels let the bottom layer show through in hardware. */
void cps1_device_display_init(void)
{
    cps1_device_display_config_layer(CPS1_LTDC_LAYER_BOTTOM, s_panel_bottom);
    cps1_device_display_config_layer(CPS1_LTDC_LAYER_TOP, s_panel_top);

    HAL_LTDC_ConfigColorKeying(&hltdc, CPS1_LTDC_COLOR_KEY_RGB565, CPS1_LTDC_LAYER_TOP);
    HAL_LTDC_EnableColorKeying(&hltdc, CPS1_LTDC_LAYER_TOP);
}

/* Call once per video frame, after cps1_core_render_ltdc_bottom() and
 * cps1_core_run_frame_device_cost() have both produced this frame's
 * content: crops each into its panel-sized buffer and reloads LTDC so the
 * new addresses (unchanged here, since the buffers are static -- only
 * their CONTENT changes) take effect at the next vertical blank. */
void cps1_device_display_submit(cps1_engine_kind_t engine)
{
    cps1_core_crop_to_panel(cps1_core_get_ltdc_bottom_buffer(), s_panel_bottom);
    cps1_core_crop_to_panel(cps1_core_get_framebuffer(engine), s_panel_top);

    HAL_LTDC_Reload(&hltdc, LTDC_RELOAD_VERTICAL_BLANKING);
}
