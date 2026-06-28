/* DOOM (doomgeneric) homebrew port for Game & Watch / retro-go-sd.
 *
 * Runs from the .overlay_doom RAM region (shipped as /roms/homebrew/Doom.bin,
 * cached into RAM at launch by the Homebrew dispatcher). The platform layer is
 * the 6-function doomgeneric DG_ API implemented below; app_main_doom() mirrors
 * the other homebrew apps (app_main_zelda3) including savestate/resume wiring.
 *
 * GPL-2.0 (DOOM source lineage) — see external/doomgeneric.
 */
#include <odroid_system.h>
#include <stdint.h>
#include <string.h>

#include "main.h"
#include "gw_lcd.h"
#include "gw_linker.h"
#include "gw_buttons.h"
#include "gw_malloc.h"
#include "stm32h7xx_hal.h"
#include "common.h"
#include "rom_manager.h"
#include "appid.h"
#include "odroid_overlay.h"   /* odroid_overlay_cache_file_in_flash: WAD -> XIP flash */

#include "doom/main_doom.h"

/* doomgeneric core (external/doomgeneric/doomgeneric). We do NOT include
 * doomgeneric.h here because it typedefs pixel_t as uint32_t, which clashes
 * with the firmware's pixel_t (gw_lcd.h, uint8/uint16). Declare the few
 * symbols we need with explicit types instead. DOOMGENERIC_RESX/RESY come
 * from -D flags in the Makefile. */
#include "doomkeys.h"

/* DOOM renders to I_VideoBuffer (8bpp paletted, SCREENWIDTH x SCREENHEIGHT =
 * 320 x 200). The LCD runs in LUT8 mode so the LTDC CLUT (programmed from
 * DOOM's palette in I_SetPalette) does the colour; DG_DrawFrame just copies
 * the 8bpp indices. I_VideoBuffer is allocated from the LUT8 bonus pool. */
extern unsigned char *I_VideoBuffer;
void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick(void);

/* DOOM internal native savegame (g_game.c) — used for "continue"/savestate. */
extern void G_SaveGame(int slot, const char *description);
extern void G_LoadGame(const char *name);

/* Boot trace to /doom_trace.txt (firmware-side, syscalls.c). Tees stdout with an
 * f_sync per line so the last step survives a silent hard fault. */
extern void doom_trace_begin(void);
extern void doom_trace_end(void);

#define DOOM_WAD_PATH      "/roms/homebrew/DOOM1.WAD"
#define DOOM_FB_X_OFFSET   ((ODROID_SCREEN_WIDTH  - DOOMGENERIC_RESX) / 2)   /* 0  */
#define DOOM_FB_Y_OFFSET   ((ODROID_SCREEN_HEIGHT - DOOMGENERIC_RESY) / 2)   /* 20 */

/* WAD served from memory-mapped (XIP) flash. app_main_doom() caches the
 * shareware IWAD into external flash once at launch (CRC-cached across
 * launches) and publishes the mapped pointer + size here; W_AddFile
 * (Core/Src/porting/doom/w_wad.c) then sets wad_file->mapped so DOOM serves
 * every lump in place instead of Z_Malloc-copying it into the small,
 * fragmented zone. On cache failure these stay NULL/0 and DOOM falls back to
 * reading lumps from SD (today's behaviour) with no regression.
 * (next-hack flash-resident-WAD technique, ref next-hack/nRF52840Doom.) */
uint8_t  *g_doom_wad_mapped = NULL;
uint32_t  g_doom_wad_size   = 0;

/* ------------------------------------------------------------------ */
/* Input: translate the G&W buttons into a queue of DOOM key events.   */
/* ------------------------------------------------------------------ */
static uint32_t doom_prev_buttons;

typedef struct { uint16_t mask; unsigned char key; } doom_keymap_t;

/* Conservative mapping for the ~6 usable G&W buttons. Tunable. */
static const doom_keymap_t DOOM_KEYMAP[] = {
    { B_Up,     KEY_UPARROW    },
    { B_Down,   KEY_DOWNARROW  },
    { B_Left,   KEY_LEFTARROW  },
    { B_Right,  KEY_RIGHTARROW },
    { B_A,      KEY_FIRE       },
    { B_B,      KEY_USE        },
    { B_GAME,   KEY_RSHIFT     },  /* run */
    { B_TIME,   KEY_TAB        },  /* automap */
    { B_START,  KEY_ENTER      },
    { B_SELECT, KEY_ESCAPE     },
};
#define DOOM_KEYMAP_COUNT (sizeof(DOOM_KEYMAP) / sizeof(DOOM_KEYMAP[0]))

void DG_Init(void)
{
    doom_prev_buttons = 0;
}

/* Returns 1 and fills *pressed/*doomKey for the next changed button, else 0.
 * printf-free: this runs in the first-tic input poll where DOOM HardFaulted,
 * and a veneer'd printf from this RAM-overlay function can corrupt the next
 * compare (proven on Lynx, commit f442284d). Crash location comes from the
 * BSOD fault-tee (syscalls.c), not from per-call logging. */
int DG_GetKey(int *pressed, unsigned char *doomKey)
{
    uint32_t now = buttons_get();
    uint32_t changed = now ^ doom_prev_buttons;
    if (!changed)
        return 0;

    for (unsigned i = 0; i < DOOM_KEYMAP_COUNT; i++) {
        uint16_t mask = DOOM_KEYMAP[i].mask;
        if (changed & mask) {
            *pressed = (now & mask) ? 1 : 0;
            *doomKey = DOOM_KEYMAP[i].key;
            doom_prev_buttons ^= mask;   /* consume just this bit */
            return 1;
        }
    }
    doom_prev_buttons = now;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Bonus pool: LUT8 mode frees ~146KB of the LCD pool (the second RGB565   */
/* framebuffer). We hand that to I_VideoBuffer (64KB) and the WAD lumpinfo  */
/* so the DOOM zone keeps the full RAM_EMU pool. Simple bump allocator with */
/* a RAM_EMU fallback so nothing breaks if the pool is exhausted/absent.    */
/* ------------------------------------------------------------------ */
static uint8_t *doom_bonus_base = NULL;
size_t          doom_bonus_size = 0;   /* non-static: read by I_Error MEM report */
size_t          doom_bonus_used = 0;

void *doom_bonus_alloc(size_t n)
{
    n = (n + 3u) & ~(size_t)3u;
    if (doom_bonus_base && doom_bonus_used + n <= doom_bonus_size) {
        void *p = doom_bonus_base + doom_bonus_used;
        doom_bonus_used += n;
        return p;
    }
    return ram_malloc(n);   /* fallback: zone-adjacent RAM, as before */
}

/* ------------------------------------------------------------------ */
/* Video: DOOM renders to I_VideoBuffer (8bpp paletted, RESX x RESY).  */
/* In LUT8 mode the LCD framebuffer is also 8bpp and the LTDC CLUT does */
/* the colour, so we just copy the indices into the letterboxed middle  */
/* of the active buffer (borders cleared once at init). No conversion.  */
/* ------------------------------------------------------------------ */
void DG_DrawFrame(void)
{
    const uint8_t *src = I_VideoBuffer;   /* 8bpp paletted, RESX x RESY */

    if (src == NULL)
        return;

    /* Don't touch the framebuffer while the previous swap is still being applied
     * at vblank. Issuing another draw+swap mid-reload corrupts the active/inactive
     * buffer state: we draw into the buffer currently on screen (garbled/torn) or
     * flip to an unwritten buffer (black). Every working core gates on this; DOOM
     * was the only one swapping unconditionally. Present the next tic instead --
     * I_VideoBuffer keeps the latest frame. */
    if (lcd_is_swap_pending())
        return;

    /* Software palette like the host harness: look up rgb565_palette[index] for
     * each 8bpp pixel and write RGB565 into the letterboxed middle of the active
     * buffer (X_OFFSET is 0 so rows are contiguous). LCD stays in RGB565 mode. */
    extern uint16_t rgb565_palette[256];
    uint16_t *dst = (uint16_t *)lcd_get_active_buffer()
                  + (size_t)DOOM_FB_Y_OFFSET * ODROID_SCREEN_WIDTH;
    const size_t pixels = (size_t)DOOMGENERIC_RESX * DOOMGENERIC_RESY;
    for (size_t i = 0; i < pixels; ++i)
        dst[i] = rgb565_palette[src[i]];
    wdog_refresh();
    lcd_swap();
}

/* ------------------------------------------------------------------ */
/* Timing.                                                             */
/* ------------------------------------------------------------------ */
uint32_t DG_GetTicksMs(void)
{
    return HAL_GetTick();
}

void DG_SleepMs(uint32_t ms)
{
    (void)ms;   /* printf-free: was a tick-probe; see Lynx 6c93f023 lesson. */
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

/* ------------------------------------------------------------------ */
/* Savestate / resume — route retro-go save/load to DOOM's native      */
/* savegame so "continue" restores the in-progress game.               */
/* ------------------------------------------------------------------ */
static bool doom_system_SaveState(const char *savePathName)
{
    (void)savePathName;
    odroid_audio_mute(true);
    /* DOOM savegame is deferred via gameaction; pump a tick to flush it. */
    G_SaveGame(0, "retro-go");
    doomgeneric_Tick();
    odroid_audio_mute(false);
    return true;
}

static bool doom_system_LoadState(const char *savePathName)
{
    (void)savePathName;
    odroid_audio_mute(true);
    G_LoadGame("doomsav0.dsg");
    doomgeneric_Tick();
    odroid_audio_mute(false);
    return true;
}

static void *Screenshot(void)
{
    return lcd_get_active_buffer();
}

/* ------------------------------------------------------------------ */
/* Heap: route the engine's malloc/calloc/realloc/free (renamed to      */
/* doom__* via doom_redefines) to the 724KB RAM_EMU overlay region. The */
/* firmware's newlib _sbrk heap is only ~87KB — far too small for DOOM's */
/* zone. Bump allocator: free is a no-op; realloc copies via a size      */
/* header. (newlib's own internal mallocs are NOT renamed, so they keep  */
/* using the small _sbrk heap.)                                          */
/* ------------------------------------------------------------------ */
void *doom__malloc(size_t n)
{
    uint32_t *p = (uint32_t *)ram_malloc(n + sizeof(uint32_t));
    if (!p) return NULL;
    *p = (uint32_t)n;
    return p + 1;
}
void *doom__calloc(size_t a, size_t b)
{
    size_t n = a * b;
    void *p = doom__malloc(n);
    if (p) memset(p, 0, n);
    return p;
}
void doom__free(void *p) { (void)p; }
void *doom__realloc(void *p, size_t n)
{
    void *q = doom__malloc(n);
    if (p && q) {
        uint32_t old = ((uint32_t *)p)[-1];
        memcpy(q, p, old < n ? old : n);
    }
    return q;
}

/* ------------------------------------------------------------------ */
/* Entry point.                                                        */
/* ------------------------------------------------------------------ */
int app_main_doom(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    /* Trace already begun by the launcher (rg_emulators.c) BEFORE the XIP
     * cache step so a fault there is captured too; do NOT re-begin here (that
     * would CREATE_ALWAYS-truncate and drop the cache breadcrumbs). */
    printf("DOOM start (build trace)\n");
    ram_start = (uint32_t)&_OVERLAY_DOOM_BSS_END;

    /* Configure the LCD pixel format BEFORE odroid_system_init turns the
     * backlight/LTDC scanout on. Reconfiguring the LTDC (pixel format +
     * framebuffer address + vblank reload) while it is ALREADY actively
     * scanning wedges the display on the wrong/cleared buffer -- the panel
     * stays black even though the engine renders fine. Working cores (zelda3)
     * never call lcd_setup_framebuffers; DOOM must do it while the panel is
     * still idle, then let odroid_system_init bring the backlight up on a
     * stable LTDC. DG_DrawFrame does the 8bpp->RGB565 palette lookup in
     * software, so there is no LTDC CLUT/LUT8 path. I_VideoBuffer (64KB) and
     * the WAD lumpinfo come from the zone-adjacent RAM_EMU pool. */
    lcd_setup_framebuffers(LCD_MODE_RGB565);
    doom_bonus_base = NULL;   /* no LUT8 bonus pool -> ram_malloc fallback */
    doom_bonus_size = 0;
    doom_bonus_used = 0;

    odroid_system_init(APPID_HOMEBREW, 11025);
    odroid_system_emu_init(&doom_system_LoadState, &doom_system_SaveState,
                           &Screenshot, NULL, NULL, NULL);

    lcd_clear_buffers();      /* clear both buffers (letterbox borders) */

    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }

    /* Cache the shareware IWAD into memory-mapped (XIP) flash before booting
     * doomgeneric, so W_AddFile can serve lumps in place (see globals above).
     * First launch copies ~4 MB to flash; subsequent launches are instant. */
    g_doom_wad_mapped = odroid_overlay_cache_file_in_flash(DOOM_WAD_PATH,
                                                           &g_doom_wad_size, false);
    printf("[doom] WAD flash-cache: ptr=%p size=%lu\n",
           (void *)g_doom_wad_mapped, (unsigned long)g_doom_wad_size);

    /* Boot doomgeneric with the shareware IWAD from SD/flash. */
    static char *argv[] = { "doom", "-iwad", DOOM_WAD_PATH };
    printf("[doom] doomgeneric_Create...\n");
    doomgeneric_Create(3, argv);
    printf("[doom] doomgeneric_Create returned OK\n");

    if (load_state)
        odroid_system_emu_load_state(save_slot);
    else
        lcd_clear_buffers();

    odroid_gamepad_state_t joystick;
    while (true) {
        wdog_refresh();

        /* printf-free main loop, like zelda3 and the other working cores
         * (Lynx 6c93f023): a veneer'd printf per frame from this RAM overlay
         * can corrupt the next compare. A first-tic fault is captured by the
         * BSOD fault-tee (syscalls.c) with pc/lr, which is how the original
         * thintriangle_guy corruption was decoded -- no per-tick log needed. */

        /* Retro-go menu + savestate hotkeys (PAUSE+A save / PAUSE+B load,
         * brightness/volume, quit) via the registered Save/Load callbacks. */
        odroid_input_read_gamepad(&joystick);
        odroid_dialog_choice_t options[] = { ODROID_DIALOG_CHOICE_LAST };
        void _repaint(void) { DG_DrawFrame(); }
        common_emu_input_loop(&joystick, options, &_repaint);

        /* One DOOM frame; gameplay input flows through DG_GetKey(). */
        doomgeneric_Tick();
    }
    return 0;
}
