/* nhdoom DEVICE port: homebrew entry point (replaces src/main.c + doom_iwad.c).
 *
 * Mirrors the host blueprint (linux/nhdoom/src/host_main.c) on real hardware:
 *   1. odroid_system init, like the other homebrew apps,
 *   2. cache the MCUDoomWadUtil-converted IWAD (doom1.mcu.wad) into memory-
 *      mapped OCTOSPI XIP flash (0x90000000) via odroid_overlay_cache_file_in_flash,
 *   3. publish the three pointer-packing bases the shadow i_memory.h reads:
 *        - nh_ram_ptr_base    = linker-pinned 256KB short-pointer window
 *                               (nh_ram_window_base, 256KB-aligned in RAM_EMU),
 *        - nh_ext_flash_base  = XIP WAD base (minus 4, so WAD_ADDRESS hits the
 *                               IWAD at offset 0 of the .mcu.wad),
 *        - nh_flash_addr_base = internal-flash cache base (unused on device:
 *                               the WAD is read-only XIP, save writes are no-ops),
 *   4. initGraphics, Z_Init, InitGlobals, D_DoomMain (auto-warps to a level via
 *      the DEBUG_SETUP/AUTOSTART path; never returns).
 *
 * The iwad base is set AFTER the cache call (a non-constant init), exactly like
 * host_main.c — so the upstream doom_iwad.c static initializer is NOT used.
 *
 * GPL-2.0 (see external/nh-doom, next-hack/nRF52840Doom lineage).
 */
/* stm32h7xx_hal.h FIRST so CMSIS (__CMSIS_GCC_H) is established before any
 * firmware header pulls the shadow main.h -> shadow nrf.h, whose intrinsics are
 * then suppressed (see nhdoom/include/nrf.h). */
#include "stm32h7xx_hal.h"
#include <stdint.h>
#include <stdio.h>
#include <odroid_system.h>
#include "common.h"
#include "appid.h"
#include "gw_lcd.h"
#include "gw_malloc.h"
#include "odroid_overlay.h"
#include "nhdoom/main_nhdoom.h"

/* runtime pointer-packing bases consumed by the shadow i_memory.h. */
unsigned long nh_ram_ptr_base;
unsigned long nh_ext_flash_base;
unsigned long nh_ext_flash_size;
unsigned long nh_flash_addr_base;

/* WAD access pointers (normally doom_iwad.c, whose static init derives from the
 * now-runtime WAD_ADDRESS). doom_iwad.c is EXCLUDED from the build; we own them
 * and set them once the WAD is XIP-cached. */
const unsigned char *doom_iwad = 0;
const unsigned int  *p_doom_iwad_len = 0;
static unsigned int  nh_wad_len;

/* small scratch internal-flash-cache region (kept tiny; the engine's flash
 * cache path is not exercised because the WAD is served in place from XIP). */
static uint8_t nh_flash_cache_stub[64] __attribute__((aligned(4)));

extern char nh_ram_window_base[];        /* linker symbol (256KB-aligned)  */
extern uint32_t _OVERLAY_NHDOOM_BSS_END; /* end of nhdoom RAM footprint     */

extern void initGraphics(void);
extern void Z_Init(void);
extern void InitGlobals(void);
extern void D_DoomMain(void);

#define NHDOOM_WAD_PATH "/roms/homebrew/doom1.mcu.wad"

int app_main_nhdoom(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)load_state; (void)save_slot;
    printf("NHDOOM (next-hack engine) start\n");

    /* ram_malloc (used by device_video for the displayData double buffer) bumps
     * from the end of the nhdoom RAM footprint, keeping it out of the window. */
    ram_start = (uint32_t)&_OVERLAY_NHDOOM_BSS_END;

    odroid_system_init(APPID_HOMEBREW, 11025);
    odroid_system_emu_init(NULL, NULL, NULL, NULL, NULL, NULL);

    if (start_paused)
        odroid_audio_mute(true);

    /* Cache the converted IWAD into memory-mapped XIP flash. */
    uint32_t wad_size = 0;
    uint8_t *wad = odroid_overlay_cache_file_in_flash(NHDOOM_WAD_PATH, &wad_size, false);
    printf("[nhdoom] WAD flash-cache: ptr=%p size=%lu\n",
           (void *)wad, (unsigned long)wad_size);

    /* Publish the pointer-packing bases (mirror host_main.c). */
    nh_ram_ptr_base    = (unsigned long)(uintptr_t)nh_ram_window_base;
    nh_flash_addr_base = (unsigned long)(uintptr_t)nh_flash_cache_stub;
    if (wad) {
        nh_ext_flash_base = (unsigned long)(uintptr_t)wad - 4;  /* WAD_ADDRESS = wad */
        nh_ext_flash_size = (unsigned long)wad_size + 4;
        nh_wad_len        = wad_size;
        doom_iwad         = (const unsigned char *)wad;
        p_doom_iwad_len   = &nh_wad_len;
    }

    printf("[nhdoom] RAM_PTR_BASE=%#lx EXT_FLASH_BASE=%#lx FLASH_ADDRESS=%#lx\n",
           nh_ram_ptr_base, nh_ext_flash_base, nh_flash_addr_base);

    initGraphics();
    Z_Init();
    InitGlobals();
    D_DoomMain();   /* never returns */
    return 0;
}
