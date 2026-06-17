// Video playback loop — see video_play.h.
//
// Demux an AVI, decode + pace its MJPEG frames to the LCD and feed its MP3 audio
// to the SAI ring (the Music app's brick-safe path). Adapts to the file's
// size/rate, drops frames when behind, and exposes a retro transport overlay
// (play/pause icon, speed, progress) that auto-hides. Truly undecodable input
// returns VID_UNPLAYABLE so the app can show a message.

#include "video_play.h"
#include "avi.h"
#include "video_decode.h"
#include "video_audio.h"
#include "gw_lcd.h"
#include "gw_audio.h"           // audio_start_playing / music_audio_* / AUDIO_BUFFER_LENGTH
#include "common.h"             // common_emu_sound_get_volume / common_emu_state
#include "main.h"               // HAL_GetTick / HAL_Delay / wdog_refresh
#include <odroid_system.h>
#include "rg_i18n.h"
#include "odroid_overlay.h"
#include "gui.h"                // curr_colors
#include <string.h>
#include <stdio.h>

#define OSD_MS       2200       // transport overlay visible after a key press
#define OSD_H        26
#define SEEK_SECONDS 5

// Speed steps: 0=0.5x, 1=1x, 2=2x  (interval = frame_ms * den / num).
static const int SPD_NUM[] = { 1, 1, 2 };
static const int SPD_DEN[] = { 2, 1, 1 };
static const char *SPD_LBL[] = { "x0.5", "x1", "x2" };

// Audio is synced only at 1x (the ring plays at a fixed 48kHz; speed-changing the
// video would desync it), and muted while paused. This tells the ISR the current
// volume + play state without touching the ring.
static void apply_audio(int spd, bool paused)
{
    bool live = (spd == 1) && !paused;
    music_audio_set(live ? common_emu_sound_get_volume() : 0, live ? 1 : 0);
}

// Pump one AVI audio chunk (MP3) into the decode->ring path. Small chunks (one or
// a few MP3 frames), read sequentially on the shared file handle — never racing
// the video read (FF_FS_TINY-safe).
static void feed_audio(avi_t *a, long sz)
{
    uint8_t buf[2048];
    while (sz > 0) {
        wdog_refresh();
        size_t want = sz > (long)sizeof buf ? sizeof buf : (size_t)sz;
        size_t got = fread(buf, 1, want, a->f);
        if (got == 0) break;
        video_audio_feed(buf, (int)got);
        sz -= (long)got;
    }
}

static void draw_osd(const avi_t *a, int vframe, int spd, bool paused)
{
    uint16_t bg = curr_colors->bg_c, fg = curr_colors->main_c, ac = curr_colors->sel_c;
    int y = GW_LCD_HEIGHT - OSD_H;
    odroid_overlay_draw_fill_rect(0, y, GW_LCD_WIDTH, OSD_H, bg);
    odroid_overlay_draw_fill_rect(0, y, GW_LCD_WIDTH, 1, ac);

    // play / pause glyph (Geometric Shapes — the device font's "free icons")
    const char *icon = paused ? "\xE2\x96\xAE\xE2\x96\xAE" : "\xE2\x96\xB6";   // ▮▮ / ▶
    i18n_draw_text_line(8, y + 6, 24, icon, fg, bg, 1);
    i18n_draw_text_line(32, y + 6, 30, SPD_LBL[spd], fg, bg, 0);

    char vbuf[12];
    snprintf(vbuf, sizeof vbuf, "\xE2\x99\xAA%d", odroid_audio_volume_get());   // ♪N volume
    i18n_draw_text_line(64, y + 6, 32, vbuf, fg, bg, 0);

    // elapsed / total time on the right (M:SS/M:SS — ASCII, baked font, no SD)
    int fms   = avi_frame_ms(a);
    int cur_s = vframe * fms / 1000;
    int tot_s = a->total_frames > 0 ? a->total_frames * fms / 1000 : 0;
    char tbuf[24];
    if (tot_s > 0)
        snprintf(tbuf, sizeof tbuf, "%d:%02d/%d:%02d", cur_s / 60, cur_s % 60, tot_s / 60, tot_s % 60);
    else
        snprintf(tbuf, sizeof tbuf, "%d:%02d", cur_s / 60, cur_s % 60);
    int tw = i18n_get_text_width(tbuf);
    i18n_draw_text_line(GW_LCD_WIDTH - tw - 6, y + 6, tw + 4, tbuf, fg, bg, 0);

    // progress bar fills the middle, between the volume readout and the time
    int bx = 100, bw = GW_LCD_WIDTH - bx - tw - 16, bh = 6, byy = y + 10;
    if (bw > 0) {
        odroid_overlay_draw_fill_rect(bx, byy, bw, bh, curr_colors->dis_c);
        if (a->total_frames > 0) {
            int fillw = (int)((long)bw * vframe / a->total_frames);
            if (fillw > bw) fillw = bw;
            odroid_overlay_draw_fill_rect(bx, byy, fillw, bh, ac);
        }
    }
}

vid_result_t video_play(const char *path)
{
    avi_t a;
    if (!avi_open(&a, path))
        return VID_UNPLAYABLE;

    const int frame_ms = avi_frame_ms(&a);
    const int seek_frames = frame_ms > 0 ? (SEEK_SECONDS * 1000) / frame_ms : 24 * SEEK_SECONDS;

    odroid_gamepad_state_t joy, prev;
    odroid_input_read_gamepad(&prev);   // seed with the CURRENT state so the A press
                                        // that launched playback isn't seen as a new
                                        // edge (which would instantly pause it)

    // Audio bring-up: attach the ring to the SAI ISR, start the DMA, hand the
    // buffer to this app, then set the initial volume/play state (1x, unpaused).
    common_emu_state.skip_frames = 0;
    common_emu_state.pause_frames = 0;
    video_audio_start();
    audio_start_playing(AUDIO_BUFFER_LENGTH);
    music_audio_enable(1);
    apply_audio(1, false);

    int  spd = 1;                  // 1x
    int  vframe = 0;
    bool decoded_any = false, stopped = false, paused = false;
    uint32_t next_due = HAL_GetTick();
    uint32_t osd_until = HAL_GetTick() + OSD_MS;   // show briefly on entry
    bool need_seek = false; int seek_target = 0;

    long sz;
    avi_kind_t k;
    while ((k = avi_next(&a, &sz)) != AVI_END) {
        wdog_refresh();

        odroid_input_read_gamepad(&joy);
        #define HIT(b) (joy.values[b] && !prev.values[b])
        bool any_press = false;
        for (int b = 0; b < ODROID_INPUT_MAX; b++) if (joy.values[b] && !prev.values[b]) any_press = true;

        if (HIT(ODROID_INPUT_B)) { stopped = true; prev = joy; break; }
        if (HIT(ODROID_INPUT_A)) { paused = !paused; apply_audio(spd, paused); }
        if (HIT(ODROID_INPUT_SELECT)) { spd = (spd + 1) % 3; next_due = HAL_GetTick(); apply_audio(spd, paused); }
        if (HIT(ODROID_INPUT_RIGHT)) { need_seek = true; seek_target = vframe + seek_frames; }
        if (HIT(ODROID_INPUT_LEFT))  { need_seek = true; seek_target = vframe - seek_frames; }
        if (HIT(ODROID_INPUT_UP))   { int v = odroid_audio_volume_get(); if (v < ODROID_AUDIO_VOLUME_MAX) odroid_audio_volume_set(v + 1); apply_audio(spd, paused); }
        if (HIT(ODROID_INPUT_DOWN)) { int v = odroid_audio_volume_get(); if (v > 0) odroid_audio_volume_set(v - 1); apply_audio(spd, paused); }
        if (any_press) osd_until = HAL_GetTick() + OSD_MS;
        prev = joy;

        if (need_seek) {
            need_seek = false;
            avi_seek_frame(&a, seek_target);
            vframe = seek_target < 0 ? 0 : seek_target;
            video_audio_stop();        // drop audio buffered from the old position
            next_due = HAL_GetTick();
            continue;
        }

        if (paused) {
            uint32_t pstart = HAL_GetTick();
            while (paused) {
                wdog_refresh();
                odroid_input_read_gamepad(&joy);
                if (HIT(ODROID_INPUT_A)) paused = false;
                if (HIT(ODROID_INPUT_B)) { stopped = true; paused = false; }
                prev = joy;
                HAL_Delay(20);
            }
            next_due += HAL_GetTick() - pstart;     // shift the clock past the pause
            apply_audio(spd, paused);               // unmute on resume
            if (stopped) break;
        }

        if (k == AVI_AUDIO) {
            if (spd == 1 && !paused) feed_audio(&a, sz);   // audio is synced only at 1x
            continue;
        }

        // VIDEO frame.
        uint32_t interval = (uint32_t)frame_ms * SPD_DEN[spd] / SPD_NUM[spd];
        uint32_t now = HAL_GetTick();
        vframe++;

        if (now > next_due + interval) {            // behind -> drop (don't decode)
            next_due += interval;
            continue;
        }

        if (video_decode_frame(a.f, sz, lcd_get_active_buffer(),
                               GW_LCD_WIDTH, GW_LCD_HEIGHT))
            decoded_any = true;
        if ((int32_t)(HAL_GetTick() - osd_until) < 0)
            draw_osd(&a, vframe, spd, paused);

        while ((int32_t)(HAL_GetTick() - next_due) < 0) {
            wdog_refresh();
            HAL_Delay(1);
        }
        lcd_swap();
        next_due += interval;
    }

    music_audio_set(0, 0);       // ISR outputs silence
    video_audio_stop();          // drain the ring
    music_audio_enable(0);       // hand the DMA buffer back to the system
    avi_close(&a);
    if (!decoded_any) return VID_UNPLAYABLE;
    return stopped ? VID_STOPPED : VID_OK;
}
