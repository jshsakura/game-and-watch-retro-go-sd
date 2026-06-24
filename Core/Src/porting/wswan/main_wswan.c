#include <odroid_system.h>

#include <assert.h>
#include "gw_lcd.h"
#include "gw_linker.h"
#include "gw_buttons.h"
#include "rom_manager.h"
#include "common.h"
#include "gw_malloc.h"
#include "rg_storage.h"
#include "odroid_overlay.h"
#include "appid.h"
#include "bilinear.h"
#include "rg_i18n.h"

/* oswan (WonderSwan / Color) core. We forward-declare the handful of entry
 * points we use instead of including WS.h, which exposes generic globals
 * (IO, Page, ...) that risk clashing with the firmware / CMSIS headers. */
void WsInit(void);
void WsReset(void);
uint32_t WsRun(void);
extern uint16_t FrameBuffer[240 * 144];          /* render target (NOSDL_FB) */
int  ws_create_from_flash(const uint8_t *data, uint32_t size); /* G&W ROM loader */

/* APU ring buffer (WSApu.c). apuBufGetLock/Unlock live in the excluded SDL
 * backend, so we drain the ring directly. */
#define WS_SND_RNGSIZE (8 * 512)   /* must match oswan SND_RNGSIZE */
extern int16_t sndbuffer[2][WS_SND_RNGSIZE];
extern int32_t rBuf, wBuf;
extern int ws_render_enabled;   /* WSRender.c: 0 = skip per-scanline pixel work */

/* File-based savestate (WSFileio.c on the fork). Forward-declared to avoid
 * pulling oswan's headers (SDL/CMSIS clashes) into the front-end. */
extern uint32_t WsSaveStateToFile(FILE *fp);
extern uint32_t WsLoadStateFromFile(FILE *fp);

/* Referenced by oswan's WsLoadEeprom (defined in the excluded SDL front-end). */
char gameName[512];

/* End-of-frame present hook. oswan's Interrupt() calls graphics_paint() at
 * vblank; the SDL front-end blits there. We blit FrameBuffer ourselves in the
 * app loop, so this is a no-op. Namespaced (ws__graphics_paint via
 * wswan_redefines) so it does not collide with RACE's graphics_paint. */
void graphics_paint(void) { }

/* oswan's APU (SOUND_ON) calls into its SDL sound back-end, which we don't
 * compile. We drain the sample ring ourselves in ws_pcm_submit, so these are
 * no-ops. */
void Sound_APU_Start(void) { }
void Sound_APU_End(void)   { }
void Sound_APUClose(void)  { }
void Pause_Sound(void)     { }

/* WonderSwan native screen: 224x144, rendered into FrameBuffer at row stride
 * 240 with an 8-pixel left margin (see RefreshLine). */
#define WIDTH       320
#define WS_WIDTH    (224)
#define WS_HEIGHT   (144)
#define WS_STRIDE   (240)
#define WS_XOFF     (8)
/* WonderSwan's native refresh. The earlier freeze was NOT the rate - it was
 * WsRun rendering every frame; now that ws_render_enabled skips render on
 * dropped frames the pacer keeps up at 75, so run at native speed. */
#define WS_FPS      (75)

#define WS_SAMPLE_RATE         (44100)
#define WS_AUDIO_BUFFER_LENGTH (WS_SAMPLE_RATE / WS_FPS)
static int16_t audioBuffer_ws[WS_AUDIO_BUFFER_LENGTH * 2];

static odroid_video_frame_t video_frame = {WS_WIDTH, WS_HEIGHT, WS_STRIDE * 2, 2, 0xFF, -1, NULL, NULL, 0, {}};

/* The G&W input register oswan reads via WsInputGetState(). Bit layout (see
 * WS.c): Y1-4 = bits 0-3, X1-4 (D-pad) = bits 4-7, OPTION=8, START=9, A=10,
 * B=11. We drive the X-pad as the main D-pad. */
uint32_t WsInputGetState(void)
{
    odroid_gamepad_state_t joystick;
    odroid_input_read_gamepad(&joystick);
    uint32_t s = 0;
    /* Drive ONLY the X-pad (the horizontal-orientation D-pad). Mirroring onto
     * the Y-pad as well breaks games that use the Y-pad as action buttons -
     * left/right then doubled as Y2/Y4 and the character jittered/diagonal'd. */
    if (joystick.values[ODROID_INPUT_UP])     s |= 0x0010; /* X1 up */
    if (joystick.values[ODROID_INPUT_RIGHT])  s |= 0x0020; /* X2 right */
    if (joystick.values[ODROID_INPUT_DOWN])   s |= 0x0040; /* X3 down */
    if (joystick.values[ODROID_INPUT_LEFT])   s |= 0x0080; /* X4 left */
    if (joystick.values[ODROID_INPUT_A])      s |= 0x0400; /* WS A = red A */
    if (joystick.values[ODROID_INPUT_B])      s |= 0x0800; /* WS B = B button */
    /* WS START on the GAME button and the dedicated START button (Zelda ed.). */
    if (joystick.values[ODROID_INPUT_START] ||
        joystick.values[ODROID_INPUT_X])      s |= 0x0200; /* WS START */
    if (joystick.values[ODROID_INPUT_SELECT] ||
        joystick.values[ODROID_INPUT_Y])      s |= 0x0100; /* WS OPTION = TIME / SELECT */
    return s;
}

static void blit_emulator(void);

/* Savestates are not wired for v1 (oswan's WsSaveState is FILE-based). */
static bool SaveState(const char *savePathName) {
    FILE *file = fopen(savePathName, "wb");
    if (file == NULL)
        return false;
    uint32_t err = WsSaveStateToFile(file);
    fclose(file);
    return err == 0;
}

static bool LoadState(const char *savePathName) {
    FILE *file = fopen(savePathName, "rb");
    if (file == NULL)
        return false;
    uint32_t err = WsLoadStateFromFile(file);
    fclose(file);
    if (err == 0)
        lcd_clear_active_buffer();
    return err == 0;
}

static void *Screenshot(void)
{
    lcd_wait_for_vblank();
    lcd_clear_active_buffer();
    blit_emulator();
    return lcd_get_active_buffer();
}

void ws_pcm_submit(void)
{
    int16_t *dst = audio_get_active_buffer();
    uint16_t dst_len = audio_get_buffer_length();
    if (common_emu_sound_loop_is_muted()) {
        return;
    }
    int32_t factor = common_emu_sound_get_volume();
    /* Samples the APU produced this frame. The APU rate (~HBlank*MULT) doesn't
     * equal the fixed SAI rate, so resample this frame's samples to fill the
     * whole output buffer at the right pitch and consume them all - avoids the
     * underrun stretch that made the music lag/click. */
    int32_t avail = wBuf - rBuf;
    if (avail < 0) avail += WS_SND_RNGSIZE;
    if (avail < 1) {
        for (int i = 0; i < dst_len; i++) dst[i] = 0;
        return;
    }
    /* Resample exactly this frame's `avail` samples to fill `dst_len`, with
     * LINEAR INTERPOLATION. Consuming exactly `avail` per frame keeps the
     * playback speed correct (one frame of audio -> one frame of output); the
     * earlier continuous-phase version drifted and played fast. Lerp removes
     * the nearest-neighbour aliasing that made it sound crunchy. */
    uint32_t step = ((uint32_t)avail << 16) / dst_len;
    if (step == 0) step = 1;
    uint32_t pos = 0;
    /* Output the raw lerp'd APU mix - no DC/low-pass filter. The filter didn't
     * help (and made it differ from the reference web oswan, which is the same
     * core with raw output). Match that: just resample + volume. */
    for (int i = 0; i < dst_len; i++) {
        uint32_t whole = pos >> 16;
        uint32_t frac  = pos & 0xFFFF;
        int32_t i0 = rBuf + (int32_t)whole;
        if (i0 >= WS_SND_RNGSIZE) i0 -= WS_SND_RNGSIZE;
        int32_t i1 = i0 + 1;
        if (i1 >= WS_SND_RNGSIZE) i1 -= WS_SND_RNGSIZE;
        int32_t s0 = (sndbuffer[0][i0] + sndbuffer[1][i0]) >> 1;
        int32_t s1 = (sndbuffer[0][i1] + sndbuffer[1][i1]) >> 1;
        int32_t s  = s0 + (((s1 - s0) * (int32_t)frac) >> 16);   /* lerp */
        dst[i] = (int16_t)((s * factor) >> 8);  /* factor 0..255 volume */
        pos += step;
    }
    rBuf += avail;
    if (rBuf >= WS_SND_RNGSIZE) rBuf -= WS_SND_RNGSIZE;
}

__attribute__((optimize("unroll-loops")))
static inline void screen_blit_nn(int32_t dest_width, int32_t dest_height)
{
    int w1 = WS_WIDTH;
    int h1 = WS_HEIGHT;
    int w2 = dest_width;
    int h2 = dest_height;

    int x_ratio = (int)((w1 << 16) / w2) + 1;
    int y_ratio = (int)((h1 << 16) / h2) + 1;
    int hpad = (320 - dest_width) / 2;
    int wpad = (240 - dest_height) / 2;

    uint16_t *dest = lcd_get_active_buffer();

    for (int i = 0; i < h2; i++) {
        for (int j = 0; j < w2; j++) {
            int x2 = ((j * x_ratio) >> 16);
            int y2 = ((i * y_ratio) >> 16);
            /* source row stride is WS_STRIDE with an 8px left margin */
            dest[((i + wpad) * WIDTH) + j + hpad] = FrameBuffer[(y2 * WS_STRIDE) + x2 + WS_XOFF];
        }
    }
}

static void blit_emulator(void)
{
    odroid_display_scaling_t scaling = odroid_display_get_scaling_mode();
    odroid_display_filter_t filtering = odroid_display_get_filter_mode();

    (void)filtering;
    switch (scaling) {
    case ODROID_DISPLAY_SCALING_OFF:
        /* 224x144 centred leaves a black border; clear first so a previous
         * full-screen frame's pixels don't stay as garbage around the image. */
        lcd_clear_active_buffer();
        screen_blit_nn(WS_WIDTH, WS_HEIGHT);
        break;
    case ODROID_DISPLAY_SCALING_FIT: {
        /* Aspect-preserving: WonderSwan 224:144 (1.56) is WIDER than the 320:240
         * LCD (1.33), so fill the width (320) and letterbox the height. 320x206
         * with ~17px black bars top/bottom — image keeps correct proportions
         * (vs FULL which stretches). Clear first so the bars aren't stale pixels. */
        int fit_h = (WS_HEIGHT * 320 + WS_WIDTH / 2) / WS_WIDTH;   /* ~206 */
        lcd_clear_active_buffer();
        screen_blit_nn(320, fit_h);
        break;
    }
    case ODROID_DISPLAY_SCALING_FULL:
    case ODROID_DISPLAY_SCALING_CUSTOM:
    default:
        /* Stretch to the full 320x240 (fills the screen, slight vertical stretch).
         * Always nearest-neighbour: the old bilinear (SOFT filter) path fed imlib a
         * PACKED 224-wide source while FrameBuffer's real row stride is WS_STRIDE
         * (240), drifting 16px/row and shearing the picture. NN reads the stride
         * correctly and is crisp; soft filtering is disabled until done vs the true stride. */
        screen_blit_nn(320, 240);
        break;
    }
}

static void blit(void) {
    blit_emulator();
    common_ingame_overlay();
}

static size_t ws_getromdata(unsigned char **data) {
    uint32_t size = 0;
    const unsigned char *src = odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &size, false);
    if (src == NULL || size == 0) {
        *data = NULL;
        return 0;
    }
    *data = (unsigned char *)src;
    return size;
}

void app_main_wswan(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    uint32_t rom_length = 0;
    unsigned char *rom_ptr = NULL;
    odroid_gamepad_state_t joystick;
    odroid_dialog_choice_t options[] = {
        ODROID_DIALOG_CHOICE_LAST
    };

    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }
    common_emu_state.frame_time_10us = (uint16_t)(100000 / WS_FPS + 0.5f);
    lcd_set_refresh_rate(WS_FPS);

    video_frame.buffer = FrameBuffer;

    odroid_system_init(APPID_WSWAN, WS_SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, &Screenshot, NULL, NULL, NULL);

    /* First-run default = FIT (global OFF = tiny native, too small here). FIT
     * fills the screen while preserving the 224:144 aspect ratio — sensible,
     * without FULL's stretch. Saved to the shared global setting; the user can
     * change it afterwards. */
    if (odroid_display_get_scaling_mode() == ODROID_DISPLAY_SCALING_OFF)
        odroid_display_set_scaling_mode(ODROID_DISPLAY_SCALING_FIT);

    audio_start_playing(WS_AUDIO_BUFFER_LENGTH);

    WsInit();

    rom_length = ws_getromdata(&rom_ptr);
    ws_create_from_flash(rom_ptr, (uint32_t)rom_length);
    WsReset();

    if (load_state) {
        odroid_system_emu_load_state(save_slot);
    } else {
        lcd_clear_buffers();
    }

    while (1)
    {
        wdog_refresh();

        bool drawFrame = common_emu_frame_loop();

        /* Guarantee the panel refreshes at least every few frames even if the
         * frame-skip integrator latches. One Piece WSC's sound-DMA stalls pin
         * skip_frames=2 during heavy stretches, and the 1x audio-DMA pacing
         * below keeps elapsed ~= frame_time so the integrator never decays ->
         * render+present stay skipped and the screen looks frozen until PAUSE
         * resets it. Forcing a draw on the 6th consecutive skip un-freezes it. */
        static int ws_skipped_run = 0;
        if (drawFrame) {
            ws_skipped_run = 0;
        } else if (++ws_skipped_run >= 6) {
            drawFrame = true;
            ws_skipped_run = 0;
        }

        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, &blit);
        common_emu_input_loop_handle_turbo(&joystick);

        /* Skip per-scanline rendering on frames we won't display, so the
         * emulator can keep pace (WsRun always renders otherwise). */
        ws_render_enabled = drawFrame;
        WsRun();

        if (drawFrame) {
            blit();
            lcd_swap();
        }

        ws_pcm_submit();

        /* Pace the loop by the audio DMA UNCONDITIONALLY. common_emu_sound_sync
         * skips this wait when skip_frames>0, which let heavy games (render
         * skipped -> CPU spare) run ahead of real time and play audio at
         * fast-forward speed. Waiting for one DMA tick every frame caps the
         * emulator at real time so the pitch is correct; on frames that genuinely
         * overran, the DMA has already advanced so this passes through with no
         * extra delay (render-skip still kicks in via common_emu_frame_loop). */
        /* ...but ONLY at 1x. During a user speed-up (e.g. 1.5x), the loop must run
         * faster than real time; pinning it to the audio DMA here would make
         * common_emu_frame_loop (which targets the faster rate) see the loop as
         * permanently 'behind' -> skip_frames stays maxed -> the screen never
         * redraws (looks frozen). So skip the wait while speeding up. */
        if (odroid_system_get_app()->speedupEnabled == SPEEDUP_1x) {
            static uint32_t ws_last_dma = 0;
            if (ws_last_dma == 0) ws_last_dma = dma_counter;
            while (dma_counter == ws_last_dma)
                cpumon_sleep();
            ws_last_dma = dma_counter;
        }
    }
}
