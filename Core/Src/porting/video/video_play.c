// Video playback loop — see video_play.h.
//
// Demux an AVI, hardware-decode + pace its MJPEG frames to the LCD and feed its
// MP3 audio to the SAI ring. Transport: A pause / B stop / SELECT speed / volume
// on UP-DOWN / LEFT-RIGHT tap = ±5s, hold = scrub (freeze the picture, drag the
// bar, one seek on release). A translucent overlay with a Music-style knob slider
// + elapsed/total time auto-hides. Truly undecodable input returns VID_UNPLAYABLE.

#include "video_play.h"
#include "avi.h"
#include "video_decode.h"
#include "video_audio.h"
#include "gw_lcd.h"
#include "gw_audio.h"           // audio_start_playing / music_audio_* / AUDIO_BUFFER_LENGTH
#include "common.h"             // common_emu_sound_get_volume / common_emu_state
#include "music_cover.h"        // g_scratch (frozen-frame store while scrubbing)
#include "main.h"               // HAL_GetTick / HAL_Delay / wdog_refresh
#include <odroid_system.h>
#include "rg_i18n.h"
#include "odroid_overlay.h"     // odroid_overlay_settings_menu + dialog choices
#include "gui.h"                // curr_colors
#include <string.h>
#include <stdio.h>

#define OSD_MS       2200       // transport overlay visible after a key press
#define OSD_H        30         // overlay band height (room for the knob slider)
#define HOLD_MS      280        // press longer than this => scrub instead of a tap-seek
#define SEEK_SECONDS 5
#define FB_PX        (GW_LCD_WIDTH * GW_LCD_HEIGHT)

// Speed steps: 0=0.5x, 1=1x, 2=2x  (interval = frame_ms * den / num).
static const int SPD_NUM[] = { 1, 1, 2 };
static const int SPD_DEN[] = { 2, 1, 1 };
static const char *SPD_LBL[] = { "x0.5", "x1", "x2" };

extern uint8_t g_scratch[];

// Audio is synced only at 1x (the ring plays at a fixed 48kHz; speed-changing the
// video would desync it) and muted while paused.
static void apply_audio(int spd, bool paused)
{
    bool live = (spd == 1) && !paused;
    music_audio_set(live ? common_emu_sound_get_volume() : 0, live ? 1 : 0);
}

// Pump one AVI audio chunk (MP3) into the decode->ring path.
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

// Mute + flush the audio across a seek so the SAI ISR can't buzz on the stale /
// underrunning ring while the demuxer walks to the target.
static void seek_to(avi_t *a, int frame, int spd, bool paused, uint32_t *next_due)
{
    music_audio_set(0, 0);
    avi_seek_frame(a, frame);
    video_audio_stop();
    apply_audio(spd, paused);
    *next_due = HAL_GetTick();
}

// --- overlay drawing --------------------------------------------------------

// Blend RGB565 a toward b by n/16.
static inline uint16_t vmix(uint16_t a, uint16_t b, int n)
{
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    return (uint16_t)((((ar + (((br - ar) * n) >> 4)) << 11)) |
                      (((ag + (((bg - ag) * n) >> 4)) << 5)) |
                       ((ab + (((bb - ab) * n) >> 4))));
}

// Filled circle (the slider knob).
static void dot(uint16_t *fb, int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy; if (y < 0 || y >= GW_LCD_HEIGHT) continue;
        for (int dx = -r; dx <= r; dx++) {
            int x = cx + dx; if (x < 0 || x >= GW_LCD_WIDTH) continue;
            if (dx * dx + dy * dy <= r * r) fb[y * GW_LCD_WIDTH + x] = c;
        }
    }
}

static void fmt_time(char *out, int total_frames_at, int frame_ms)
{
    int s = total_frames_at * frame_ms / 1000;
    snprintf(out, 8, "%d:%02d", s / 60, s % 60);
}

// Transport overlay: a translucent band (the video shows through, dimmed) with a
// play/pause glyph, the Music-deck knob slider and elapsed/total time. When
// scrub_frame >= 0 the slider previews that position (knob turns white).
static void draw_osd(const avi_t *a, int spd, bool paused, int scrub_frame, int frame_ms)
{
    uint16_t *fb = lcd_get_active_buffer();
    uint16_t bg = curr_colors->bg_c, fg = curr_colors->main_c, accent = curr_colors->sel_c;
    int y0 = GW_LCD_HEIGHT - OSD_H, cy = y0 + OSD_H / 2;
    bool scrub = scrub_frame >= 0;
    int total = a->total_frames > 0 ? a->total_frames : 0;
    int pos = scrub ? scrub_frame : a->cur_frame;

    // translucent overlay band + a thin top edge (subtle, not a hard line)
    for (int y = y0; y < GW_LCD_HEIGHT; y++) {
        uint16_t *row = fb + y * GW_LCD_WIDTH;
        for (int x = 0; x < GW_LCD_WIDTH; x++) row[x] = vmix(row[x], 0x0000, 11);
    }
    for (int x = 0; x < GW_LCD_WIDTH; x++) fb[y0 * GW_LCD_WIDTH + x] = vmix(bg, accent, 6);

    // left cluster: play/pause glyph (+ speed tag when not 1x)
    const char *icon = paused ? "\xE2\x96\xAE\xE2\x96\xAE" : "\xE2\x96\xB6";   // ▮▮ / ▶
    int lx = 8;
    i18n_draw_text_line(lx, cy - 6, 18, icon, fg, 0, 1);
    lx += 22;
    if (spd != 1) {
        i18n_draw_text_line(lx, cy - 6, 28, SPD_LBL[spd], accent, 0, 1);
        lx += i18n_get_text_width(SPD_LBL[spd]) + 6;
    }

    // elapsed (left of the bar) and total (far right)
    char te[8], tt[8];
    fmt_time(te, pos, frame_ms);
    fmt_time(tt, total, frame_ms);
    int ew = i18n_get_text_width(te), tw = i18n_get_text_width(tt);
    i18n_draw_text_line(lx, cy - 6, ew + 4, te, scrub ? accent : fg, 0, 1);
    int tx = GW_LCD_WIDTH - tw - 8;
    i18n_draw_text_line(tx, cy - 6, tw + 4, tt, fg, 0, 1);

    // knob slider between them (3px track + accent fill + ringed knob)
    int sx = lx + ew + 10, ex = tx - 12, sw = ex - sx;
    if (sw > 12) {
        int fillw = total > 0 ? (int)((long)sw * pos / total) : 0;
        if (fillw < 0) fillw = 0; if (fillw > sw) fillw = sw;
        for (int dy = -1; dy <= 1; dy++) {
            uint16_t *row = fb + (cy + dy) * GW_LCD_WIDTH;
            for (int x = 0; x < sw; x++)    row[sx + x] = vmix(fg, bg, 9);   // track
            for (int x = 0; x < fillw; x++) row[sx + x] = accent;            // played
        }
        dot(fb, sx + fillw, cy, 5, vmix(bg, accent, 11));     // knob ring
        dot(fb, sx + fillw, cy, 3, scrub ? fg : accent);      // knob core
    }
}

// Volume popup (shown briefly while UP/DOWN adjust the level), like the Music app.
static void draw_volume(void)
{
    uint16_t *fb = lcd_get_active_buffer();
    uint16_t bg = curr_colors->bg_c, fg = curr_colors->main_c, accent = curr_colors->sel_c;
    int vol = odroid_audio_volume_get(), maxv = ODROID_AUDIO_VOLUME_MAX;
    int pw = 160, ph = 26, px = (GW_LCD_WIDTH - pw) / 2, py = 18;
    for (int y = py; y < py + ph; y++)
        for (int x = px; x < px + pw; x++)
            fb[y * GW_LCD_WIDTH + x] = vmix(fb[y * GW_LCD_WIDTH + x], 0x0000, 12);
    i18n_draw_text_line(px + 10, py + 7, 16, "\xE2\x99\xAA", fg, 0, 1);   // ♪
    int bx = px + 32, bw = pw - 44, bh = 8, by = py + (ph - bh) / 2;
    int fillw = maxv > 0 ? bw * vol / maxv : 0;
    for (int y = 0; y < bh; y++) {
        uint16_t *row = fb + (by + y) * GW_LCD_WIDTH;
        for (int x = 0; x < bw; x++)    row[bx + x] = vmix(fg, bg, 9);
        for (int x = 0; x < fillw; x++) row[bx + x] = accent;
    }
}

// --- options menu (PAUSE / SET) ---------------------------------------------

#define VTR(field, fallback) ((curr_lang && curr_lang->field) ? curr_lang->field : (fallback))
enum { VMENU_INFO = 90, VMENU_QUIT = 91, VMENU_DEBUG = 92 };

// Diagnostic overlay toggle (declared here so the options menu can flip it).
static bool g_show_debug = true;

// The shared launcher settings menu (Brightness + Volume sliders) plus a read-only
// Info line, a Debug-overlay toggle and a Quit entry. Returns the chosen id
// (VMENU_QUIT to leave); flips the debug overlay in place when that entry is picked.
static int open_video_menu(const avi_t *a)
{
    static char info[40];
    int fps = a->usec_per_frame > 0 ? (1000000 + a->usec_per_frame / 2) / a->usec_per_frame : 24;
    snprintf(info, sizeof info, "%dx%d  %dfps", a->width, a->height, fps);
    odroid_dialog_choice_t extra[] = {
        { VMENU_INFO,  VTR(s_info, "Info"),               info,                              0, NULL },
        { VMENU_DEBUG, "Debug",                           (char *)(g_show_debug ? "ON" : "OFF"), 1, NULL },
        ODROID_DIALOG_CHOICE_SEPARATOR,
        { VMENU_QUIT,  VTR(s_Quit_to_menu, "Quit to menu"), (char *)"",                       1, NULL },
        ODROID_DIALOG_CHOICE_LAST,
    };
    int r = odroid_overlay_settings_menu(extra, NULL, 0);
    if (r == VMENU_DEBUG) g_show_debug = !g_show_debug;
    return r;
}

// --- playback ---------------------------------------------------------------

// Comprehensive diagnostic for the last unplayable clip (shown on screen, '|' =
// line break). Lets one device run pinpoint where playback breaks.
extern int  g_vdec_st, g_vdec_w, g_vdec_h;
extern long g_vdec_sz, g_vdec_rc;
extern unsigned char g_vdec_b0, g_vdec_b1;
static char s_diag[200];
const char *video_last_diag(void) { return s_diag[0] ? s_diag : "unsupported / unreadable"; }

static void build_diag(const avi_t *a, int nv, int na)
{
    int fps = a->usec_per_frame > 0 ? (1000000 + a->usec_per_frame / 2) / a->usec_per_frame : 0;
    snprintf(s_diag, sizeof s_diag,
             "open %dx%d %dfps f=%d|frame %02X%02X sz=%ld|decode st=%d %dx%d rc=%ld|"
             "chunks v=%d a=%d",
             a->width, a->height, fps, a->total_frames,
             g_vdec_b0, g_vdec_b1, g_vdec_sz,
             g_vdec_st, g_vdec_w, g_vdec_h, g_vdec_rc, nv, na);
}

// Live decoder overlay (toggle in the options menu via g_show_debug): a translucent
// top panel showing what the HW JPEG path is doing — decoded/seen/audio counts, the
// last stage/return code, parsed dims and first bytes — so a stall or a bad frame is
// diagnosable at a glance without reflashing. Default on; the video shows through.
static void draw_hud(int dec_ok, int seen, int na)
{
    char l1[48], l2[48];
    snprintf(l1, sizeof l1, "dec=%d v=%d a=%d", dec_ok, seen, na);
    snprintf(l2, sizeof l2, "st=%d rc=%ld %dx%d sz=%ld %02X%02X",
             g_vdec_st, g_vdec_rc, g_vdec_w, g_vdec_h, g_vdec_sz, g_vdec_b0, g_vdec_b1);
    uint16_t *fb = lcd_get_active_buffer();
    uint16_t accent = curr_colors->sel_c;
    for (int y = 0; y < 26; y++) {                       // translucent panel (video shows through)
        uint16_t *row = fb + y * GW_LCD_WIDTH;
        for (int x = 0; x < GW_LCD_WIDTH; x++) row[x] = vmix(row[x], 0x0000, 9);
    }
    for (int x = 0; x < GW_LCD_WIDTH; x++) fb[26 * GW_LCD_WIDTH + x] = accent;   // accent edge
    i18n_draw_text_line(3, 2,  GW_LCD_WIDTH - 6, l1, accent,               0, 1);
    i18n_draw_text_line(3, 14, GW_LCD_WIDTH - 6, l2, curr_colors->main_c, 0, 1);
}

vid_result_t video_play(const char *path)
{
    avi_t a;
    // No setvbuf read-ahead: a big fully-buffered stdio buffer front-loads ~16
    // frames during open then STALLS for a full ~128KB SD refill once drained
    // (the "first frames then freeze" the device showed). The proven-smooth build
    // (27679012490) had none — each frame is read with one direct fread of its own
    // size, which newlib services as a direct large read (no per-read storm).
    if (!avi_open(&a, path, NULL, 0)) {
        snprintf(s_diag, sizeof s_diag, "avi_open FAILED");
        return VID_UNPLAYABLE;
    }
    int nv_seen = 0, na_seen = 0;
    s_diag[0] = '\0';                   // fresh diag for this clip

    video_decode_init();                // power up the hardware JPEG codec

    const int frame_ms = avi_frame_ms(&a);
    const int seek_frames = frame_ms > 0 ? (SEEK_SECONDS * 1000) / frame_ms : 24 * SEEK_SECONDS;
    int scrub_step = a.total_frames > 0 ? a.total_frames / 120 : 12;
    if (scrub_step < 1) scrub_step = 1;

    odroid_gamepad_state_t joy, prev;
    odroid_input_read_gamepad(&prev);   // seed with the CURRENT state so the A press
                                        // that launched playback isn't seen as a new edge

    common_emu_state.skip_frames = 0;
    common_emu_state.pause_frames = 0;
    video_audio_start();
    audio_start_playing(AUDIO_BUFFER_LENGTH);
    music_audio_enable(1);
    apply_audio(1, false);

    int  spd = 1, dec_ok = 0;
    bool decoded_any = false, stopped = false, paused = false;
    uint32_t next_due  = HAL_GetTick();
    bool anchored = false;              // re-anchor next_due at the 1st frame after start/seek
    uint32_t osd_until = HAL_GetTick() + OSD_MS;
    uint32_t vol_until = 0;
    bool lr_down = false; int lr_dir = 0; uint32_t lr_press = 0;

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
        if (HIT(ODROID_INPUT_SELECT)) { spd = (spd + 1) % 3; anchored = false; apply_audio(spd, paused); }
        if (HIT(ODROID_INPUT_VOLUME)) {                  // PAUSE/SET -> options menu
            music_audio_set(0, 0);
            int r = open_video_menu(&a);
            apply_audio(spd, paused);
            anchored  = false;
            osd_until = HAL_GetTick() + OSD_MS;
            odroid_input_read_gamepad(&prev);            // swallow the menu's buttons
            if (r == VMENU_QUIT) { stopped = true; break; }
            continue;
        }
        if (HIT(ODROID_INPUT_UP))   { int v = odroid_audio_volume_get(); if (v < ODROID_AUDIO_VOLUME_MAX) odroid_audio_volume_set(v + 1); apply_audio(spd, paused); vol_until = HAL_GetTick() + 1500; }
        if (HIT(ODROID_INPUT_DOWN)) { int v = odroid_audio_volume_get(); if (v > 0) odroid_audio_volume_set(v - 1); apply_audio(spd, paused); vol_until = HAL_GetTick() + 1500; }

        // LEFT/RIGHT: track press; tap (short) = ±5s, hold (long) = scrub.
        bool nowL = joy.values[ODROID_INPUT_LEFT], nowR = joy.values[ODROID_INPUT_RIGHT];
        bool lr = nowL || nowR;
        if (lr && !lr_down) { lr_down = true; lr_dir = nowR ? 1 : -1; lr_press = HAL_GetTick(); }
        bool released   = lr_down && !lr;
        bool go_scrub   = lr_down && lr && (HAL_GetTick() - lr_press > HOLD_MS);

        if (any_press) osd_until = HAL_GetTick() + OSD_MS;
        prev = joy;

        if (released) {                                  // quick tap -> skip ±5s
            lr_down = false;
            seek_to(&a, a.cur_frame + lr_dir * seek_frames, spd, paused, &next_due);
            anchored = false;
            continue;
        }

        if (go_scrub) {                                  // hold -> freeze + drag the bar
            lr_down = false;
            int scrub_frame = a.cur_frame;
            music_audio_set(0, 0);                       // silence while scrubbing
            memcpy(g_scratch, lcd_get_active_buffer(), FB_PX * sizeof(uint16_t));   // freeze
            for (;;) {
                wdog_refresh();
                odroid_input_read_gamepad(&joy);
                if (HIT(ODROID_INPUT_B)) { stopped = true; prev = joy; break; }
                bool sL = joy.values[ODROID_INPUT_LEFT], sR = joy.values[ODROID_INPUT_RIGHT];
                prev = joy;
                if (!sL && !sR) {                        // release -> one seek
                    seek_to(&a, scrub_frame, spd, paused, &next_due);
                    anchored = false;
                    break;
                }
                scrub_frame += (sR ? 1 : -1) * scrub_step;
                if (scrub_frame < 0) scrub_frame = 0;
                if (a.total_frames > 0 && scrub_frame >= a.total_frames) scrub_frame = a.total_frames - 1;
                memcpy(lcd_get_active_buffer(), g_scratch, FB_PX * sizeof(uint16_t));
                draw_osd(&a, spd, paused, scrub_frame, frame_ms);
                lcd_swap();
                HAL_Delay(16);
            }
            if (stopped) break;
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
            anchored = false;                            // re-anchor the schedule after the pause
            apply_audio(spd, paused);
            if (stopped) break;
        }

        if (k == AVI_AUDIO) {
            na_seen++;
            if (spd == 1 && !paused) feed_audio(&a, sz);
            continue;
        }

        // VIDEO frame: audio is the master clock — it MUST play at real time or it
        // drifts/stutters ("음악이 밀려"). When we fall behind schedule (the SD can't
        // sustain reading every frame at the full rate) SKIP this frame's decode AND
        // its data read, handing that bandwidth/time back to the audio so the ring
        // never starves. Video just loses a few frames (e.g. 30->~20fps), audio stays
        // locked. The schedule is anchored at the FIRST frame after start/seek so the
        // audio preamble / seek walk never poisons it (that was the old all-drop bug).
        nv_seen++;
        uint32_t interval = (uint32_t)frame_ms * SPD_DEN[spd] / SPD_NUM[spd];
        if (!anchored) { next_due = HAL_GetTick(); anchored = true; }

        if ((int32_t)(HAL_GetTick() - next_due) > (int32_t)interval) {   // behind -> drop frame
            next_due += interval;                                        // advance the schedule
            if ((int32_t)(HAL_GetTick() - next_due) > (int32_t)(interval * 16))
                next_due = HAL_GetTick();                                // hard resync after a stall
            continue;                                                    // skip decode+read; audio flows
        }

        if (video_decode_frame(a.f, sz, lcd_get_active_buffer(), GW_LCD_WIDTH, GW_LCD_HEIGHT)) {
            decoded_any = true; dec_ok++;
        } else if (nv_seen >= 30) {               // first 30 frames all failed to DECODE -> report
            build_diag(&a, nv_seen, na_seen);
            stopped = true; break;
        }
        if ((int32_t)(HAL_GetTick() - osd_until) < 0) {
            draw_osd(&a, spd, paused, -1, frame_ms);
            if (g_show_debug) draw_hud(dec_ok, nv_seen, na_seen);   // debug rides with the OSD
        }
        if ((int32_t)(HAL_GetTick() - vol_until) < 0)
            draw_volume();

        while ((int32_t)(HAL_GetTick() - next_due) < 0) { wdog_refresh(); HAL_Delay(1); }
        lcd_swap();
        next_due += interval;
    }

    music_audio_set(0, 0);
    video_audio_stop();
    music_audio_enable(0);
    audio_stop_playing();        // halt the SAI DMA so it can't loop the stale buffer (exit buzz)
    video_decode_deinit();
    avi_close(&a);
    if (!decoded_any) {
        if (!s_diag[0]) build_diag(&a, nv_seen, na_seen);   // (early-bail already filled it)
        return VID_UNPLAYABLE;
    }
    return stopped ? VID_STOPPED : VID_OK;
}
