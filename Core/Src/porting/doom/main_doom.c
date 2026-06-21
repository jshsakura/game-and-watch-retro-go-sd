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

extern uint32_t *DG_ScreenBuffer;   /* ARGB8888, DOOMGENERIC_RESX x RESY */
void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick(void);

/* DOOM internal native savegame (g_game.c) — used for "continue"/savestate. */
extern void G_SaveGame(int slot, const char *description);
extern void G_LoadGame(const char *name);

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
/* Video: blit DG_ScreenBuffer (ARGB8888, RESX x RESY) -> RGB565 LCD,  */
/* letterboxed vertically (200 -> 240).                                */
/* ------------------------------------------------------------------ */
static inline uint16_t argb8888_to_rgb565(uint32_t p)
{
    uint8_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void DG_DrawFrame(void)
{
    uint16_t *dst = lcd_get_active_buffer();
    const uint32_t *src = DG_ScreenBuffer;

    for (int y = 0; y < DOOMGENERIC_RESY; y++) {
        uint16_t *row = dst + (size_t)(y + DOOM_FB_Y_OFFSET) * ODROID_SCREEN_WIDTH + DOOM_FB_X_OFFSET;
        for (int x = 0; x < DOOMGENERIC_RESX; x++)
            row[x] = argb8888_to_rgb565(*src++);
        wdog_refresh();
    }
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
/* Entry point.                                                        */
/* ------------------------------------------------------------------ */
int app_main_doom(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    printf("DOOM start\n");
    ram_start = (uint32_t)&_OVERLAY_DOOM_BSS_END;

    odroid_system_init(APPID_HOMEBREW, 11025);
    odroid_system_emu_init(&doom_system_LoadState, &doom_system_SaveState,
                           &Screenshot, NULL, NULL, NULL);

    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }

    /* Boot doomgeneric with the shareware IWAD from SD/flash. */
    static char *argv[] = { "doom", "-iwad", DOOM_WAD_PATH };
    doomgeneric_Create(3, argv);

    if (load_state)
        odroid_system_emu_load_state(save_slot);
    else
        lcd_clear_buffers();

    odroid_gamepad_state_t joystick;
    while (true) {
        wdog_refresh();

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
