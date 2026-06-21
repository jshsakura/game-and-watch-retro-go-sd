/* Wolfenstein 3D (autobalance/Wolf3D-STM32 engine) homebrew port for
 * Game & Watch / retro-go-sd. Runs from the .overlay_wolf3d RAM region
 * (shipped as /roms/homebrew/Wolf3D.bin). Shareware data: /roms/homebrew/WOLF3D/*.WL1.
 *
 * This firmware-side glue deliberately does NOT include the engine headers
 * (wl_def.h etc.) — those redefine byte/boolean/pixel types that clash with the
 * firmware headers. We talk to the engine via a few explicit extern symbols.
 *
 * GPL-2.0 (id Software Wolf3D lineage) — see external/wolf3d-stm32.
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

#include "wolf3d/main_wolf3d.h"

/* ---- engine entry + input service (defined in the Wolf3D engine TUs) ---- */
extern void wl_main(void);
extern void INL_KeyService(int data);   /* PS/2 set-2 byte stream */

/* Wolf3D screen is 320x200; letterbox into the 320x240 LCD. */
#define WOLF_W 320
#define WOLF_H 200
#define WOLF_Y_OFFSET ((ODROID_SCREEN_HEIGHT - WOLF_H) / 2)   /* 20 */

/* PS/2 set-2 scancodes (match id_in.h). Release = 0xF0 prefix then code. */
#define PS2_RELEASE 0xF0
#define SC_UP    0x75
#define SC_DOWN  0x72
#define SC_LEFT  0x6B
#define SC_RIGHT 0x74
#define SC_CTRL  0x14   /* fire   */
#define SC_SPACE 0x29   /* open   */
#define SC_LSHIFT 0x12  /* run    */
#define SC_ALT   0x11   /* strafe */
#define SC_ENTER 0x5A
#define SC_ESC   0x76

typedef struct { uint16_t mask; uint8_t sc; } wolf_keymap_t;
static const wolf_keymap_t WOLF_KEYMAP[] = {
    { B_Up,    SC_UP    },
    { B_Down,  SC_DOWN  },
    { B_Left,  SC_LEFT  },
    { B_Right, SC_RIGHT },
    { B_A,     SC_CTRL  },   /* fire   */
    { B_B,     SC_SPACE },   /* open   */
    { B_GAME,  SC_LSHIFT},   /* run    */
    { B_TIME,  SC_ALT   },   /* strafe */
    { B_START, SC_ENTER },
    { B_SELECT,SC_ESC   },
};
#define WOLF_KEYMAP_COUNT (sizeof(WOLF_KEYMAP) / sizeof(WOLF_KEYMAP[0]))

static uint32_t wolf_prev_buttons;

static void wolf_input_poll(void)
{
    uint32_t now = buttons_get();
    uint32_t changed = now ^ wolf_prev_buttons;
    if (!changed) return;

    for (unsigned i = 0; i < WOLF_KEYMAP_COUNT; i++) {
        uint16_t mask = WOLF_KEYMAP[i].mask;
        if (!(changed & mask)) continue;
        if (now & mask) {                 /* press */
            INL_KeyService(WOLF_KEYMAP[i].sc);
        } else {                          /* release */
            INL_KeyService(PS2_RELEASE);
            INL_KeyService(WOLF_KEYMAP[i].sc);
        }
    }
    wolf_prev_buttons = now;
}

/* ---- retro-go pause menu (savestate hotkeys, brightness/volume, quit) ---- */
static void wolf_menu_poll(void)
{
    odroid_gamepad_state_t joystick;
    odroid_input_read_gamepad(&joystick);
    odroid_dialog_choice_t options[] = { ODROID_DIALOG_CHOICE_LAST };
    void _repaint(void) { }   /* menu overlay redraws itself */
    common_emu_input_loop(&joystick, options, &_repaint);
}

/* ---- frame present: software 8-bit palette -> RGB565, called by the engine's
 * VH_UpdateScreen each frame (see wolf_vh.c). ---- */
void gw_wolf_present(const uint8_t *fb, const uint32_t *pal888)
{
    static uint16_t lut[256];
    for (int i = 0; i < 256; i++) {
        uint32_t c = pal888[i];
        lut[i] = (uint16_t)((((c >> 16) & 0xF8) << 8) |
                            (((c >> 8)  & 0xFC) << 3) |
                            ((c & 0xFF) >> 3));
    }

    uint16_t *lcd = lcd_get_active_buffer();
    /* clear the letterbox bars (top/bottom 20 rows) */
    memset(lcd, 0, (size_t)WOLF_Y_OFFSET * ODROID_SCREEN_WIDTH * sizeof(uint16_t));
    memset(lcd + (size_t)(WOLF_Y_OFFSET + WOLF_H) * ODROID_SCREEN_WIDTH, 0,
           (size_t)WOLF_Y_OFFSET * ODROID_SCREEN_WIDTH * sizeof(uint16_t));

    for (int y = 0; y < WOLF_H; y++) {
        uint16_t *row = lcd + (size_t)(y + WOLF_Y_OFFSET) * ODROID_SCREEN_WIDTH;
        const uint8_t *s = fb + (size_t)y * WOLF_W;
        for (int x = 0; x < WOLF_W; x++)
            row[x] = lut[s[x]];
        wdog_refresh();
    }
    lcd_swap();

    wolf_input_poll();
    wolf_menu_poll();
}

/* ---- driver shims the engine expects (timing) ---- */
uint32_t get_ticks_ms(void) { return HAL_GetTick(); }

void delay_ms(uint32_t ms)
{
    uint32_t end = HAL_GetTick() + ms;
    while (HAL_GetTick() < end)
        wdog_refresh();
}

/* ---- savestate / resume (registered; Wolf3D native save wiring TODO) ---- */
static bool wolf_system_SaveState(const char *path) { (void)path; return false; }
static bool wolf_system_LoadState(const char *path) { (void)path; return false; }
static void *Screenshot(void) { return lcd_get_active_buffer(); }

/* Heap: route the engine's malloc/calloc/realloc/free (renamed to wolf3d__*
 * via wolf3d_redefines) to the 724KB RAM_EMU overlay region; the newlib _sbrk
 * heap (~87KB) is too small. Bump allocator: free is a no-op; realloc copies
 * via a size header. */
void *wolf3d__malloc(size_t n)
{
    uint32_t *p = (uint32_t *)ram_malloc(n + sizeof(uint32_t));
    if (!p) return NULL;
    *p = (uint32_t)n;
    return p + 1;
}
void *wolf3d__calloc(size_t a, size_t b)
{
    size_t n = a * b;
    void *p = wolf3d__malloc(n);
    if (p) memset(p, 0, n);
    return p;
}
void wolf3d__free(void *p) { (void)p; }
void *wolf3d__realloc(void *p, size_t n)
{
    void *q = wolf3d__malloc(n);
    if (p && q) {
        uint32_t old = ((uint32_t *)p)[-1];
        memcpy(q, p, old < n ? old : n);
    }
    return q;
}

int app_main_wolf3d(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)load_state; (void)save_slot;
    printf("Wolf3D start\n");
    ram_start = (uint32_t)&_OVERLAY_WOLF3D_BSS_END;

    odroid_system_init(APPID_HOMEBREW, 7042);   /* Wolf3D sound rate */
    odroid_system_emu_init(&wolf_system_LoadState, &wolf_system_SaveState,
                           &Screenshot, NULL, NULL, NULL);

    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }

    wolf_prev_buttons = 0;
    lcd_clear_buffers();

    /* Runs the engine's own loop forever (title -> menu -> game). Frame present,
     * input and the pause menu are driven from gw_wolf_present each frame. */
    wl_main();
    return 0;
}
