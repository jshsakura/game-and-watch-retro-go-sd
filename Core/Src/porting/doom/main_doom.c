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

/* Returns 1 and fills *pressed/*doomKey for the next changed button, else 0. */
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
    uint8_t *dst = (uint8_t *)lcd_get_active_buffer();
    const uint8_t *src = I_VideoBuffer;   /* 8bpp paletted, RESX x RESY */

    if (src == NULL)
        return;

    /* X_OFFSET is 0 (RESX == screen width), so rows are contiguous and we
     * can copy the whole frame into the vertical centre in one shot. */
    memcpy(dst + (size_t)DOOM_FB_Y_OFFSET * ODROID_SCREEN_WIDTH,
           src, (size_t)DOOMGENERIC_RESX * DOOMGENERIC_RESY);
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
    uint32_t end = HAL_GetTick() + ms;
    while (HAL_GetTick() < end)
        wdog_refresh();
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
    doom_trace_begin();
    /* Keep the WWDG from resetting us during slow cold-XIP compute bursts (see
     * the EWI handler in stm32h7xx_hal_msp.c). Cleared by the full reset on app
     * switch. */
    extern volatile int g_doom_running;
    g_doom_running = 1;
    printf("DOOM start (build trace)\n");
    ram_start = (uint32_t)&_OVERLAY_DOOM_BSS_END;

    odroid_system_init(APPID_HOMEBREW, 11025);
    odroid_system_emu_init(&doom_system_LoadState, &doom_system_SaveState,
                           &Screenshot, NULL, NULL, NULL);

    /* DOOM is natively 8bpp paletted. Switch the LCD to LUT8: the LTDC does
     * palette lookup in hardware (set up in I_SetPalette via lcd_set_clut),
     * the framebuffers shrink to 154KB, and the freed ~146KB bonus pool is
     * handed to I_VideoBuffer + the WAD lumpinfo (see doom_bonus_alloc) so the
     * DOOM zone keeps the full RAM_EMU pool. odroid_system_switch_app() resets
     * the LCD back to RGB565 when we quit, so the launcher is unaffected. */
    lcd_setup_framebuffers(LCD_MODE_LUT8);
    {
        uint8_t *bp = NULL; size_t bs = 0;
        lcd_get_bonus_pool(&bp, &bs);
        doom_bonus_base = bp;
        doom_bonus_size = bs;
        doom_bonus_used = 0;
    }
    lcd_clear_buffers();   /* clear both LUT8 buffers (letterbox borders) */

    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }

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
    int trace_tick = 0;
    while (true) {
        wdog_refresh();

        /* Trace the first frames: a silent hard fault during the first level
         * load / render shows up as the last "tick N" line on the SD card. */
        if (trace_tick < 40) printf("[doom] tick %d\n", trace_tick++);

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
