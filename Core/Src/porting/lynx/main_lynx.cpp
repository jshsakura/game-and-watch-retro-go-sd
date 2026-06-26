extern "C"
{
#include <odroid_system.h>
#include <string.h>
#include <stdio.h>

#include "main.h"
#include "gw_lcd.h"
#include "gw_linker.h"
#include "gw_buttons.h"
#include "rg_i18n.h"
#include "common.h"
#include "rom_manager.h"
#include "appid.h"
#include "cpp_init_array.h"
#ifndef GNW_DISABLE_COMPRESSION
#include "lzma.h"
#endif
#include "heap.hpp"
void sd_trace_begin(const char *path); /* diagnostic SD log tee (syscalls.c) */
}

#include <handy.h>

// Lynx native screen is 160x102. We render the core into a single RGB565
// framebuffer in the overlay BSS, then 2x2 nearest-scale it (320x204) and
// center it vertically on the 320x240 LCD.
#define LYNX_FPS                60
#define AUDIO_LYNX_SAMPLE_RATE  HANDY_AUDIO_SAMPLE_FREQ /* 32000 */

static CSystem *lynx = NULL;

static uint16_t lynx_framebuffer[HANDY_SCREEN_WIDTH * HANDY_SCREEN_HEIGHT];
static SWORD    lynx_audio_buffer[HANDY_AUDIO_BUFFER_LENGTH];

static void blit();

static bool LoadState(const char *savePathName)
{
    if (lynx == NULL)
        return false;
    FILE *fp = fopen(savePathName, "rb");
    if (fp == NULL)
        return false;
    bool ret = lynx->ContextLoad(fp);
    fclose(fp);
    if (!ret)
        lynx->Reset();
    return ret;
}

static bool SaveState(const char *savePathName)
{
    if (lynx == NULL)
        return false;
    FILE *fp = fopen(savePathName, "wb");
    if (fp == NULL)
        return false;
    bool ret = lynx->ContextSave(fp);
    fclose(fp);
    return ret;
}

static void *Screenshot()
{
    lcd_wait_for_vblank();
    lcd_clear_active_buffer();
    blit();
    return lcd_get_active_buffer();
}

#ifndef GNW_DISABLE_COMPRESSION
// Memory to handle compressed roms
#define ROM_BUFF_LENGTH 524288 // 512kB (max bank-switched Lynx cart)
static uint8_t rom_memory[ROM_BUFF_LENGTH];
#endif

static size_t getromdata(unsigned char **data)
{
    /* Always hand the core a flash-resident (XIP) pointer — never heap-copy
     * the ROM. Handy's CCart memcpy's the banks it needs into its own buffers,
     * so copying the (up to 512 KB) Lynx ROM into the overlay C++ heap here is
     * pure waste: ROM(≈320K) + cart banks(≈320K) + 64K RAM overflowed the heap
     * and HardFaulted on load. lzma is unused in this build. */
    uint32_t size = 0;
    *data = (unsigned char *)odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &size, false);
    return size;
}

// 2x2 nearest scale of the 160x102 core framebuffer onto the centered
// 320x204 region of the 320x240 LCD back buffer.
static void blit()
{
    const uint16_t *src = lynx_framebuffer;
    uint16_t *out = (uint16_t *)lcd_get_active_buffer();
    const int y_offset = (GW_LCD_HEIGHT - HANDY_SCREEN_HEIGHT * 2) / 2; // 18

    for (int sy = 0; sy < HANDY_SCREEN_HEIGHT; sy++)
    {
        uint16_t *row0 = out + (y_offset + sy * 2) * GW_LCD_WIDTH;
        uint16_t *row1 = row0 + GW_LCD_WIDTH;
        const uint16_t *in = src + sy * HANDY_SCREEN_WIDTH;
        for (int sx = 0; sx < HANDY_SCREEN_WIDTH; sx++)
        {
            uint16_t c = in[sx];
            row0[sx * 2]     = c;
            row0[sx * 2 + 1] = c;
            row1[sx * 2]     = c;
            row1[sx * 2 + 1] = c;
        }
    }
}

static void sound_store()
{
    if (common_emu_sound_loop_is_muted())
    {
        gAudioBufferPointer = 0;
        return;
    }

    int32_t factor = common_emu_sound_get_volume();
    int16_t *out = audio_get_active_buffer();
    uint16_t len = audio_get_buffer_length();
    int gen = (int)(gAudioBufferPointer / 2); // stereo frames generated this frame

    for (uint16_t i = 0; i < len; i++)
    {
        int idx = (gen > 0) ? ((i < (uint16_t)gen) ? i : gen - 1) : 0;
        int32_t s = (int32_t)lynx_audio_buffer[idx * 2] + (int32_t)lynx_audio_buffer[idx * 2 + 1];
        out[i] = (int16_t)((s * factor) >> 9); // mix L+R, then apply volume/256
    }

    gAudioBufferPointer = 0;
}

static void map_buttons(odroid_gamepad_state_t *joystick)
{
    ULONG buttons = 0;
    if (joystick->values[ODROID_INPUT_UP])     buttons |= BUTTON_UP;
    if (joystick->values[ODROID_INPUT_DOWN])   buttons |= BUTTON_DOWN;
    if (joystick->values[ODROID_INPUT_LEFT])   buttons |= BUTTON_LEFT;
    if (joystick->values[ODROID_INPUT_RIGHT])  buttons |= BUTTON_RIGHT;
    if (joystick->values[ODROID_INPUT_A])      buttons |= BUTTON_A;
    if (joystick->values[ODROID_INPUT_B])      buttons |= BUTTON_B;
    if (joystick->values[ODROID_INPUT_START])  buttons |= BUTTON_PAUSE;
    if (joystick->values[ODROID_INPUT_SELECT]) buttons |= BUTTON_OPT1;
    if (joystick->values[ODROID_INPUT_X])      buttons |= BUTTON_OPT2;
    lynx->SetButtonData(buttons);
}

static void app_main_lynx_cpp(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    odroid_gamepad_state_t joystick;
    odroid_dialog_choice_t options[] = {ODROID_DIALOG_CHOICE_LAST};
    uint32_t rom_length = 0;
    uint8_t *rom_ptr = NULL;

    /* DIAGNOSTIC: tee stdout (and the BSOD fault line) to a SEPARATE SD file
     * /lynx_trace.txt (DOOM uses /doom_trace.txt) so both logs are preserved and
     * we can see exactly where Lynx dies instead of guessing. */
    sd_trace_begin("/lynx_trace.txt");
    printf("[lynx] app start (XIP cart build)\n");
    /* Surface the pre-app XIP cache/patch results (they printed to UART before
     * this trace file existed) so the SD log shows whether lynx.ro cached and
     * how many sentinel refs were patched in the flash blob + the RAM glue. */
    {
        extern uint8_t *g_lynx_ro_addr; extern unsigned int g_lynx_ro_size;
        extern int g_lynx_ro_patched; extern int g_lynx_glue_patched;
        printf("[lynx] XIP ro=%p size=%u ro_patched=%d glue_patched=%d\n",
               (void *)g_lynx_ro_addr, g_lynx_ro_size,
               g_lynx_ro_patched, g_lynx_glue_patched);
    }

    heap_itc_alloc(true);

    common_emu_state.pause_after_frames = start_paused ? 2 : 0;

    rom_length = getromdata(&rom_ptr);
    printf("[lynx] getromdata: rom_ptr=%p rom_length=%lu\n",
           (void *)rom_ptr, (unsigned long)rom_length);
    if (rom_ptr == NULL)
    {
        printf("Lynx: Failed to load ROM in flash/ram.\n");
        return;
    }

    // Init emulator (Handy renders directly to gPrimaryFrameBuffer in 565 LE)
    printf("[lynx] new CSystem...\n");
    lynx = new CSystem((const UBYTE *)rom_ptr, (ULONG)rom_length,
                       MIKIE_PIXEL_FORMAT_16BPP_565, AUDIO_LYNX_SAMPLE_RATE);

    /* DEVICE-CRITICAL: do the accept/reject check IMMEDIATELY after construction,
     * BEFORE any printf in this RAM-overlay frame. On device a RAM->flash
     * veneer'd printf here corrupts the very NEXT comparison: the DIAG proved the
     * same (lynx->mFileType == ILLEGAL) read false (eq_illegal=0) as a printf
     * ARG, yet evaluated true an instant later once the printf had run. So we
     * must test before the first post-construction printf in this frame. */
    if (lynx == NULL || lynx->mFileType == HANDY_FILETYPE_ILLEGAL)
    {
        printf("Lynx: ROM loading failed.\n");
        return;
    }
    /* Set the render/audio globals IMMEDIATELY — before any printf in this
     * RAM-overlay frame. A RAM->flash veneer'd printf here corrupts the very
     * next operation on device (proven: it flipped the fileType compare), so a
     * printf between the accept and these stores can leave gPrimaryFrameBuffer
     * pointing at garbage -> handy renders off-screen (black) and trashes
     * memory -> fault back to the menu. Stores first, log after. */
    gPrimaryFrameBuffer = (UBYTE *)lynx_framebuffer;
    gAudioBuffer = lynx_audio_buffer;
    gAudioEnabled = 1;
    printf("[lynx] CSystem ok, fb=%p (build %s %s)\n",
           (void *)gPrimaryFrameBuffer, __DATE__, __TIME__);

    uint32_t samplesPerFrame = AUDIO_LYNX_SAMPLE_RATE / LYNX_FPS;

    common_emu_state.frame_time_10us = (uint16_t)(100000 / LYNX_FPS + 0.5f);

    odroid_system_init(APPID_LYNX, AUDIO_LYNX_SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, &Screenshot, NULL, NULL, NULL);

    if (load_state)
    {
        odroid_system_emu_load_state(save_slot);
    }
    else
    {
        lcd_clear_buffers();
    }

    audio_start_playing(samplesPerFrame);

    while (1)
    {
        wdog_refresh();
        common_emu_frame_loop();
        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, &blit);

        uint8_t turbo_buttons = odroid_settings_turbo_buttons_get();
        bool turbo_a = (joystick.values[ODROID_INPUT_A] && (turbo_buttons & 1));
        bool turbo_b = (joystick.values[ODROID_INPUT_B] && (turbo_buttons & 2));
        bool turbo_button = odroid_button_turbos();
        if (turbo_a)
            joystick.values[ODROID_INPUT_A] = turbo_button;
        if (turbo_b)
            joystick.values[ODROID_INPUT_B] = !turbo_button;

        map_buttons(&joystick);

        lynx->UpdateFrame(true);

        blit();
        common_ingame_overlay();
        lcd_swap();
        sound_store();

        common_emu_sound_sync(false);
    }
}

extern "C" int app_main_lynx(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    // Call static C++ constructors now, *after* the overlay is copied into RAM.
    // Do not use __libc_init_array() as it will not work with the overlay.
    cpp_init_array(__init_array_lynx_start__, __init_array_lynx_end__);

    app_main_lynx_cpp(load_state, start_paused, save_slot);

    return 0;
}
