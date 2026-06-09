// Media Browser - homebrew app for game-and-watch-retro-go-sd
//
// Browse the SD "/media" folder like a file explorer and open files with viewers.
//   Browser:   D-pad move, A = enter folder / open .txt|.jpg|.png, B = up / exit
//   Text view: Up/Left = prev page, Down/Right/A = next page, B = back
//   Image view: B = back
//   MP3 play:  A = pause, B = back (shows cover art if found, else file name)
//   Video:     A = pause, B = back (.avi MJPEG + PCM)
//
// Implemented: browser (1), .txt (2), .jpg/.png (3), .mp3+cover (4), .avi MJPEG (5).
// Planned: .epub reader.
// See docs/MEDIA_BROWSER.md for the full roadmap.

#include <odroid_system.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "main.h"
#include "gw_lcd.h"
#include "gw_buttons.h"
#include "appid.h"
#include "rg_storage.h"
#include "odroid_overlay.h"
#include "rg_i18n.h"
#include "hw_jpeg_decoder.h"
#include "lupng.h"
#include "minimp3.h"
#include "gw_audio.h"
#include "common.h"
#include "main_media.h"

// Localized strings with English fallback (in case a language lacks the key).
#define MEDIA_HINT  ((curr_lang && curr_lang->s_media_hint)  ? curr_lang->s_media_hint  : "A:Open  B:Back")
#define MEDIA_EMPTY ((curr_lang && curr_lang->s_media_empty) ? curr_lang->s_media_empty : "(empty)")

#define MEDIA_ROOT      "/media"
#define MAX_ENTRIES     256
#define NAME_MAX_LEN    128
#define PATH_MAX_LEN    256

#define ROW_HEIGHT      16
#define HEADER_HEIGHT   20
#define FOOTER_HEIGHT   16
#define LIST_TOP        HEADER_HEIGHT
#define VISIBLE_ROWS    ((GW_LCD_HEIGHT - HEADER_HEIGHT - FOOTER_HEIGHT) / ROW_HEIGHT)

#define COLOR_BG        0x0000  // black
#define COLOR_TEXT      0xFFFF  // white  (files)
#define COLOR_DIR       0xFD20  // orange (folders)
#define COLOR_SEL_BG    0x4208  // dark gray (selected row)
#define COLOR_HEADER    0x07FF  // cyan   (path + hints)

// Text viewer (increment 2)
#define TEXT_BUF_MAX    (160 * 1024)  // max file bytes loaded (truncated beyond)
#define VIEW_ROWS       ((GW_LCD_HEIGHT - FOOTER_HEIGHT) / ROW_HEIGHT)
#define VIEW_USABLE_W   (GW_LCD_WIDTH - 8)

typedef struct {
    char name[NAME_MAX_LEN];
    bool is_dir;
} media_entry_t;

static media_entry_t entries[MAX_ENTRIES];
static int  entry_count;
static int  cursor;   // selected index into entries[]
static int  scroll;   // index of the first visible row
static char cur_path[PATH_MAX_LEN];

static int scandir_cb(const rg_scandir_t *file, void *arg)
{
    (void)arg;
    if (entry_count >= MAX_ENTRIES)
        return RG_SCANDIR_STOP;

    media_entry_t *e = &entries[entry_count++];
    strncpy(e->name, file->basename, NAME_MAX_LEN - 1);
    e->name[NAME_MAX_LEN - 1] = '\0';
    e->is_dir = file->is_dir;
    return RG_SCANDIR_CONTINUE;
}

static void scan_current(void)
{
    entry_count = 0;
    cursor = 0;
    scroll = 0;
    rg_storage_scandir(cur_path, scandir_cb, NULL,
        RG_SCANDIR_FILES | RG_SCANDIR_DIRS | RG_SCANDIR_SORT);
}

static void enter_dir(const char *name)
{
    size_t len = strlen(cur_path);
    if (len + 1 + strlen(name) + 1 >= PATH_MAX_LEN)
        return; // path would overflow; ignore

    if (strcmp(cur_path, "/") != 0)
        strcat(cur_path, "/");
    strcat(cur_path, name);
    scan_current();
}

// Returns false when already at MEDIA_ROOT (caller should exit the app).
static bool go_parent(void)
{
    if (strcmp(cur_path, MEDIA_ROOT) == 0)
        return false;

    char *slash = strrchr(cur_path, '/');
    if (!slash || slash == cur_path)
        strcpy(cur_path, MEDIA_ROOT);
    else
        *slash = '\0';

    scan_current();
    return true;
}

static void move_cursor(int delta)
{
    if (entry_count == 0)
        return;

    cursor += delta;
    if (cursor < 0)             cursor = 0;
    if (cursor >= entry_count)  cursor = entry_count - 1;

    if (cursor < scroll)
        scroll = cursor;
    if (cursor >= scroll + VISIBLE_ROWS)
        scroll = cursor - VISIBLE_ROWS + 1;
}

static void draw(void)
{
    lcd_clear_active_buffer();

    odroid_overlay_draw_text(4, 2, GW_LCD_WIDTH - 8, cur_path, COLOR_HEADER, COLOR_BG);

    if (entry_count == 0) {
        odroid_overlay_draw_text(4, LIST_TOP + 4, GW_LCD_WIDTH - 8,
            MEDIA_EMPTY, COLOR_TEXT, COLOR_BG);
    }

    for (int row = 0; row < VISIBLE_ROWS; row++) {
        int idx = scroll + row;
        if (idx >= entry_count)
            break;

        media_entry_t *e = &entries[idx];
        uint16_t y  = LIST_TOP + row * ROW_HEIGHT;
        bool selected = (idx == cursor);
        uint16_t bg = selected ? COLOR_SEL_BG : COLOR_BG;
        uint16_t fg = e->is_dir ? COLOR_DIR : COLOR_TEXT;

        char line[NAME_MAX_LEN + 4];
        if (e->is_dir)
            snprintf(line, sizeof(line), "[%s]", e->name);
        else
            snprintf(line, sizeof(line), " %s", e->name);

        odroid_overlay_draw_text(4, y, GW_LCD_WIDTH - 8, line, fg, bg);
    }

    odroid_overlay_draw_text(4, GW_LCD_HEIGHT - FOOTER_HEIGHT + 2, GW_LCD_WIDTH - 8,
        MEDIA_HINT, COLOR_HEADER, COLOR_BG);
}

// ---------------------------------------------------------------------------
// Text viewer (increment 2): page through a .txt file.
// Manual char-level wrapping + per-line draw, because i18n_draw_text mishandles
// '\n' and i18n_draw_text_line truncates rather than wraps.
// ---------------------------------------------------------------------------

static char text_buf[TEXT_BUF_MAX];
static int  text_len;

static bool has_ext(const char *name, const char *ext)
{
    size_t ln = strlen(name), le = strlen(ext);
    if (ln < le)
        return false;
    const char *s = name + ln - le;
    for (size_t i = 0; i < le; i++) {
        char a = s[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b)
            return false;
    }
    return true;
}

static int utf8_len(unsigned char c)
{
    if (c < 0x80)           return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// Read file into text_buf, dropping '\r' and mapping other control chars
// (tab etc.) to spaces. Returns sanitized length; oversized files are truncated.
static int load_text_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    int n = (int)fread(text_buf, 1, TEXT_BUF_MAX - 1, f);
    fclose(f);
    if (n < 0)
        n = 0;

    int w = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text_buf[i];
        if (c == '\r')
            continue;
        else if (c == '\n')
            text_buf[w++] = '\n';
        else if (c < 0x20)
            text_buf[w++] = ' ';
        else
            text_buf[w++] = (char)c;
    }
    text_buf[w] = '\0';
    return w;
}

static int char_px_width(const char *p, int blen)
{
    char ch[8];
    memcpy(ch, p, blen);
    ch[blen] = '\0';
    return i18n_get_text_width(ch);
}

// Draw one page starting at byte offset `start`; returns bytes consumed.
static int render_page(int start)
{
    lcd_clear_active_buffer();

    const char *base = text_buf + start;
    const char *p = base;
    char line[256];

    for (int r = 0; r < VIEW_ROWS && *p; r++) {
        int li = 0, w = 0;
        while (*p) {
            unsigned char c = (unsigned char)*p;
            if (c == '\n') { p++; break; }      // end of line, consume newline
            int blen = utf8_len(c);
            int cw = char_px_width(p, blen);
            if (w + cw > VIEW_USABLE_W)
                break;                          // doesn't fit -> wrap (keep char)
            if (li + blen >= (int)sizeof(line) - 1)
                break;
            memcpy(line + li, p, blen);
            li += blen;
            w  += cw;
            p  += blen;
        }
        line[li] = '\0';
        odroid_overlay_draw_text(4, 2 + r * ROW_HEIGHT, GW_LCD_WIDTH - 8,
            line, COLOR_TEXT, COLOR_BG);
    }

    int consumed = (int)(p - base);
    int pct = text_len ? (int)((long)(start + consumed) * 100 / text_len) : 100;
    char foot[32];
    snprintf(foot, sizeof(foot), "B   %d%%", pct);
    odroid_overlay_draw_text(4, GW_LCD_HEIGHT - FOOTER_HEIGHT + 2, GW_LCD_WIDTH - 8,
        foot, COLOR_HEADER, COLOR_BG);
    return consumed;
}

static void view_text(const char *path)
{
    static int back[2048];   // visited page-start offsets (static: keep off the stack)
    int back_n = 0;
    int top = 0, last_top = -1, consumed = 0;

    text_len = load_text_file(path);

    odroid_gamepad_state_t joy, prev;
    odroid_input_read_gamepad(&prev);   // ignore buttons still held from entering

    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&joy);
        #define VP(b) (joy.values[b] && !prev.values[b])

        if (VP(ODROID_INPUT_B))
            return;

        if (top != last_top) {
            consumed = render_page(top);
            lcd_swap();
            last_top = top;
        }

        bool more = (top + consumed) < text_len;
        if ((VP(ODROID_INPUT_DOWN) || VP(ODROID_INPUT_RIGHT) || VP(ODROID_INPUT_A)) && more) {
            if (back_n < 2048)
                back[back_n++] = top;
            top += consumed;
        } else if ((VP(ODROID_INPUT_UP) || VP(ODROID_INPUT_LEFT)) && back_n > 0) {
            top = back[--back_n];
        }

        #undef VP
        prev = joy;
        lcd_wait_for_vblank();
    }
}

// ---------------------------------------------------------------------------
// Image viewer (increment 3): JPG via the STM32 hardware JPEG codec.
// Shown at native size, centered. Images larger than the screen are reported
// instead of decoded (decode-to-frame would overflow the framebuffer).
// PNG support follows in a later step.
// ---------------------------------------------------------------------------

#define SCRATCH_MAX     (320 * 1024)  // JPEG work buffer / PNG decode pool
static uint8_t g_scratch[SCRATCH_MAX];

// Load a whole file into text_buf (the shared file buffer); returns byte count.
static int load_file(const char *path, int cap)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    int n = (int)fread(text_buf, 1, cap, f);
    fclose(f);
    return n < 0 ? 0 : n;
}

static void draw_center_msg(const char *msg)
{
    lcd_clear_active_buffer();
    odroid_overlay_draw_text(4, GW_LCD_HEIGHT / 2 - 6, GW_LCD_WIDTH - 8,
        msg, COLOR_TEXT, COLOR_BG);
    odroid_overlay_draw_text(4, GW_LCD_HEIGHT - FOOTER_HEIGHT + 2, GW_LCD_WIDTH - 8,
        "B", COLOR_HEADER, COLOR_BG);
    lcd_swap();
}

static void show_jpeg(const char *path)
{
    int n = load_file(path, TEXT_BUF_MAX);
    if (n <= 0) {
        draw_center_msg("read error");
        return;
    }

    JPEG_DecodeToFrameInit((uint32_t)g_scratch, SCRATCH_MAX);

    uint32_t w = 0, h = 0;
    if (JPEG_DecodeGetSize((uint32_t)text_buf, &w, &h) != 0) {
        JPEG_DecodeDeInit();
        draw_center_msg("not a JPEG");
        return;
    }
    if (w == 0 || h == 0 || w > GW_LCD_WIDTH || h > GW_LCD_HEIGHT) {
        JPEG_DecodeDeInit();
        char m[48];
        snprintf(m, sizeof(m), "too large: %lux%lu", (unsigned long)w, (unsigned long)h);
        draw_center_msg(m);
        return;
    }

    lcd_clear_active_buffer();
    uint16_t x = (GW_LCD_WIDTH - w) / 2;
    uint16_t y = (GW_LCD_HEIGHT - h) / 2;
    JPEG_DecodeToFrame((uint32_t)text_buf, (uint32_t)lcd_get_active_buffer(), x, y, 255);
    JPEG_DecodeDeInit();

    odroid_overlay_draw_text(4, GW_LCD_HEIGHT - FOOTER_HEIGHT + 2, GW_LCD_WIDTH - 8,
        "B", COLOR_HEADER, COLOR_BG);
    lcd_swap();
}

// --- PNG support (lupng + miniz) via a bump allocator over g_scratch, so we
// don't depend on the tiny libc heap. ---
static size_t g_scratch_off;

static void *scratch_alloc(size_t size, void *u)
{
    (void)u;
    size = (size + 3u) & ~(size_t)3u;
    if (g_scratch_off + size > SCRATCH_MAX)
        return NULL;
    void *p = g_scratch + g_scratch_off;
    g_scratch_off += size;
    return p;
}

static void scratch_free(void *p, void *u) { (void)p; (void)u; }  // reclaimed in bulk

typedef struct { const uint8_t *data; size_t size, pos; } mem_reader_t;

static size_t mem_read(void *out, size_t size, size_t count, void *u)
{
    mem_reader_t *r = (mem_reader_t *)u;
    size_t want = size * count;
    size_t avail = r->size - r->pos;
    if (want > avail) want = avail;
    memcpy(out, r->data + r->pos, want);
    r->pos += want;
    return size ? want / size : 0;
}

// Blit an 8-bit RGB(A) image to the framebuffer, centered and clipped to screen.
static void blit_rgb_centered(const uint8_t *px, int w, int h, int ch)
{
    lcd_clear_active_buffer();
    uint16_t *fb = lcd_get_active_buffer();
    int dw = w < GW_LCD_WIDTH  ? w : GW_LCD_WIDTH;
    int dh = h < GW_LCD_HEIGHT ? h : GW_LCD_HEIGHT;
    int ox = (GW_LCD_WIDTH  - dw) / 2;
    int oy = (GW_LCD_HEIGHT - dh) / 2;
    for (int y = 0; y < dh; y++) {
        const uint8_t *row = px + (size_t)y * w * ch;
        uint16_t *dst = fb + (size_t)(oy + y) * GW_LCD_WIDTH + ox;
        for (int x = 0; x < dw; x++) {
            const uint8_t *p = row + (size_t)x * ch;
            dst[x] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        }
    }
}

static void show_png(const char *path)
{
    int n = load_file(path, TEXT_BUF_MAX);
    if (n <= 0) { draw_center_msg("read error"); return; }

    mem_reader_t rd = { (const uint8_t *)text_buf, (size_t)n, 0 };
    g_scratch_off = 0;

    LuUserContext uc;
    luUserContextInitDefault(&uc);
    uc.readProc = mem_read;       uc.readProcUserPtr = &rd;
    uc.allocProc = scratch_alloc; uc.allocProcUserPtr = NULL;
    uc.freeProc = scratch_free;   uc.freeProcUserPtr = NULL;
    uc.warnProc = NULL;

    LuImage *img = luPngReadUC(&uc);
    if (!img) { draw_center_msg("PNG too big / invalid"); return; }
    if (img->depth != 8 || img->channels < 3) { draw_center_msg("unsupported PNG"); return; }

    blit_rgb_centered(img->data, img->width, img->height, img->channels);
    odroid_overlay_draw_text(4, GW_LCD_HEIGHT - FOOTER_HEIGHT + 2, GW_LCD_WIDTH - 8,
        "B", COLOR_HEADER, COLOR_BG);
    lcd_swap();
}

// Show an image and wait for B to return to the browser.
static void view_image(const char *path)
{
    if (has_ext(path, ".png"))
        show_png(path);
    else
        show_jpeg(path);

    odroid_gamepad_state_t joy, prev;
    odroid_input_read_gamepad(&prev);
    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&joy);
        if (joy.values[ODROID_INPUT_B] && !prev.values[ODROID_INPUT_B])
            return;
        prev = joy;
        lcd_wait_for_vblank();
    }
}

// ---------------------------------------------------------------------------
// MP3 player (increment 4): streaming decode via minimp3, fed to the SAI audio
// DMA (downmixed to mono, nearest-neighbour resampled to 48 kHz). A sidecar
// cover image is shown if present, else the file name. A = pause, B = back.
// ---------------------------------------------------------------------------

#define MP3_IN_BUF  (16 * 1024)

static mp3dec_t  g_mp3;
static FILE     *g_mp3_fp;
static uint8_t   g_mp3_in[MP3_IN_BUF];
static int       g_mp3_in_len, g_mp3_in_pos;
static bool      g_mp3_file_eof;
static int16_t   g_mp3_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int16_t   g_mp3_mono[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int       g_mp3_frame_n;   // mono samples currently in g_mp3_mono
static uint32_t  g_mp3_phase;     // 16.16 read index within the current frame
static uint32_t  g_mp3_step;      // (in_rate << 16) / 48000
static bool      g_mp3_eof;       // decode reached end of stream

static void mp3_refill(void)
{
    if (g_mp3_in_pos > 0) {
        int remain = g_mp3_in_len - g_mp3_in_pos;
        if (remain > 0)
            memmove(g_mp3_in, g_mp3_in + g_mp3_in_pos, remain);
        g_mp3_in_len = remain;
        g_mp3_in_pos = 0;
    }
    if (!g_mp3_file_eof) {
        int space = MP3_IN_BUF - g_mp3_in_len;
        int got = space > 0 ? (int)fread(g_mp3_in + g_mp3_in_len, 1, space, g_mp3_fp) : 0;
        if (got <= 0) g_mp3_file_eof = true;
        else          g_mp3_in_len += got;
    }
}

// Decode one MP3 frame into g_mp3_mono; returns false at end of stream.
static bool mp3_decode_frame(void)
{
    for (;;) {
        if ((g_mp3_in_len - g_mp3_in_pos) < 2048 && !g_mp3_file_eof)
            mp3_refill();

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&g_mp3, g_mp3_in + g_mp3_in_pos,
                                          g_mp3_in_len - g_mp3_in_pos, g_mp3_pcm, &info);
        g_mp3_in_pos += info.frame_bytes;

        if (samples > 0) {
            if (info.channels >= 2)
                for (int i = 0; i < samples; i++)
                    g_mp3_mono[i] = (int16_t)(((int)g_mp3_pcm[2*i] + g_mp3_pcm[2*i+1]) / 2);
            else
                for (int i = 0; i < samples; i++)
                    g_mp3_mono[i] = g_mp3_pcm[i];
            g_mp3_frame_n = samples;
            if (info.hz > 0)
                g_mp3_step = ((uint32_t)info.hz << 16) / AUDIO_SAMPLE_RATE;
            return true;
        }
        // samples == 0: skipped junk/ID3 (frame_bytes>0) or out of data
        if (info.frame_bytes == 0) {
            if (g_mp3_file_eof)
                return false;
            mp3_refill();
            if ((g_mp3_in_len - g_mp3_in_pos) == 0)
                return false;
        }
    }
}

// Produce n mono 48 kHz samples; fills silence and flags g_mp3_eof at the end.
static void mp3_produce(int16_t *out, int n)
{
    for (int k = 0; k < n; k++) {
        while ((g_mp3_phase >> 16) >= (uint32_t)g_mp3_frame_n) {
            g_mp3_phase -= (uint32_t)g_mp3_frame_n << 16;
            if (!mp3_decode_frame()) {
                g_mp3_eof = true;
                for (; k < n; k++) out[k] = 0;
                return;
            }
        }
        out[k] = g_mp3_mono[g_mp3_phase >> 16];
        g_mp3_phase += g_mp3_step;
    }
}

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// Find a cover image beside the track (same-name .jpg/.png, or cover/folder.*).
static bool find_cover(const char *mp3path, char *out, size_t outsz, bool *is_png)
{
    char base[PATH_MAX_LEN];
    snprintf(base, sizeof(base), "%s", mp3path);
    char *dot = strrchr(base, '.');
    if (dot) {
        *dot = '\0';
        snprintf(out, outsz, "%s.jpg", base);
        if (file_exists(out)) { *is_png = false; return true; }
        snprintf(out, outsz, "%s.png", base);
        if (file_exists(out)) { *is_png = true; return true; }
    }
    char dir[PATH_MAX_LEN];
    snprintf(dir, sizeof(dir), "%s", mp3path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0'; else dir[0] = '\0';

    static const char *names[] = { "cover.jpg", "folder.jpg", "cover.png" };
    for (int i = 0; i < 3; i++) {
        snprintf(out, outsz, "%s/%s", dir, names[i]);
        if (file_exists(out)) { *is_png = (strstr(names[i], ".png") != NULL); return true; }
    }
    return false;
}

static void view_mp3(const char *path)
{
    char cover[PATH_MAX_LEN];
    bool is_png = false;
    if (find_cover(path, cover, sizeof(cover), &is_png)) {
        if (is_png) show_png(cover);
        else        show_jpeg(cover);
    } else {
        const char *name = strrchr(path, '/');
        draw_center_msg(name ? name + 1 : path);
    }

    g_mp3_fp = fopen(path, "rb");
    mp3dec_init(&g_mp3);
    g_mp3_in_len = g_mp3_in_pos = 0;
    g_mp3_file_eof = (g_mp3_fp == NULL);
    g_mp3_frame_n = 0;
    g_mp3_phase = 0;
    g_mp3_step = ((uint32_t)44100 << 16) / AUDIO_SAMPLE_RATE;
    g_mp3_eof = false;

    common_emu_state.skip_frames = 0;
    common_emu_state.pause_frames = 0;
    audio_start_playing(AUDIO_BUFFER_LENGTH);

    bool paused = false;
    odroid_gamepad_state_t joy, prev;
    odroid_input_read_gamepad(&prev);

    while (!g_mp3_eof) {
        wdog_refresh();
        odroid_input_read_gamepad(&joy);
        if (joy.values[ODROID_INPUT_B] && !prev.values[ODROID_INPUT_B])
            break;
        if (joy.values[ODROID_INPUT_A] && !prev.values[ODROID_INPUT_A])
            paused = !paused;
        prev = joy;

        int16_t *buf = audio_get_active_buffer();
        int len = audio_get_buffer_length();
        if (paused || g_mp3_fp == NULL) {
            for (int i = 0; i < len; i++) buf[i] = 0;
        } else {
            mp3_produce(buf, len);
            int32_t vol = common_emu_sound_get_volume();
            for (int i = 0; i < len; i++)
                buf[i] = (int16_t)((buf[i] * vol) >> 8);
        }
        common_emu_sound_sync(false);
    }

    audio_stop_playing();
    if (g_mp3_fp) { fclose(g_mp3_fp); g_mp3_fp = NULL; }
}

// ---------------------------------------------------------------------------
// MJPEG .avi player (increment 5): demux a RIFF/AVI, decode each MJPEG frame
// with the hardware JPEG codec, and feed the PCM track to audio (mono, resampled
// to 48 kHz). Audio is the clock; a ring buffer decouples it from per-frame JPEG
// decode. A/V sync is approximate and may need on-device tuning. A = pause,
// B = back. Produce files with the ffmpeg recipe in docs/MEDIA_BROWSER.md.
// ---------------------------------------------------------------------------

#define VRING_SIZE  8192            // must be a power of two
#define VRING_MASK  (VRING_SIZE - 1)

static FILE     *g_vid_fp;
static long      g_movi_end;
static uint32_t  g_va_rate, g_va_step, g_va_phase;  // audio resample (in->48k)
static int       g_va_channels;
static int16_t   g_vring[VRING_SIZE];
static int       g_vr_head, g_vr_tail, g_vr_count;
static uint8_t   g_apcm[4096];
static int16_t   g_amono[2048];

static uint32_t favi_u32(void)
{
    uint8_t b[4];
    if (fread(b, 1, 4, g_vid_fp) != 4) return 0;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void vring_push(int16_t s)
{
    if (g_vr_count >= VRING_SIZE) return;
    g_vring[g_vr_head] = s;
    g_vr_head = (g_vr_head + 1) & VRING_MASK;
    g_vr_count++;
}

static int16_t vring_pull(void)
{
    if (g_vr_count == 0) return 0;
    int16_t s = g_vring[g_vr_tail];
    g_vr_tail = (g_vr_tail + 1) & VRING_MASK;
    g_vr_count--;
    return s;
}

// push m mono input samples, resampled (nearest) from g_va_rate to 48 kHz
static void va_push(const int16_t *in, int m)
{
    while ((g_va_phase >> 16) < (uint32_t)m) {
        vring_push(in[g_va_phase >> 16]);
        g_va_phase += g_va_step;
    }
    g_va_phase -= (uint32_t)m << 16;
}

static void wait_for_b(void)
{
    odroid_gamepad_state_t joy, prev;
    odroid_input_read_gamepad(&prev);
    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&joy);
        if (joy.values[ODROID_INPUT_B] && !prev.values[ODROID_INPUT_B]) return;
        prev = joy;
        lcd_wait_for_vblank();
    }
}

// Parse RIFF/AVI header, capture audio format, seek to the 'movi' payload.
static bool avi_open(const char *path)
{
    g_vid_fp = fopen(path, "rb");
    if (!g_vid_fp) return false;

    char cc[4];
    if (fread(cc, 1, 4, g_vid_fp) != 4 || memcmp(cc, "RIFF", 4)) return false;
    favi_u32();
    if (fread(cc, 1, 4, g_vid_fp) != 4 || memcmp(cc, "AVI ", 4)) return false;

    g_va_rate = 24000;     // sensible defaults (the documented recipe)
    g_va_channels = 1;
    bool last_auds = false;

    for (;;) {
        if (fread(cc, 1, 4, g_vid_fp) != 4) return false;
        uint32_t sz = favi_u32();

        if (!memcmp(cc, "LIST", 4)) {
            char lt[4];
            if (fread(lt, 1, 4, g_vid_fp) != 4) return false;
            if (!memcmp(lt, "movi", 4)) {
                g_movi_end = ftell(g_vid_fp) + (long)(sz - 4);
                return true;
            }
            continue;  // descend into hdrl/strl: read its children next
        }

        uint32_t padded = sz + (sz & 1);
        if (!memcmp(cc, "strh", 4)) {
            char ft[4];
            fread(ft, 1, 4, g_vid_fp);
            last_auds = !memcmp(ft, "auds", 4);
            fseek(g_vid_fp, (long)padded - 4, SEEK_CUR);
        } else if (!memcmp(cc, "strf", 4) && last_auds) {
            uint8_t wf[16];
            uint32_t r = (uint32_t)fread(wf, 1, (sz < 16 ? sz : 16), g_vid_fp);
            if (r >= 4) g_va_channels = wf[2] | (wf[3] << 8);
            if (r >= 8) g_va_rate = wf[4] | (wf[5] << 8) | (wf[6] << 16) | ((uint32_t)wf[7] << 24);
            fseek(g_vid_fp, (long)padded - (long)r, SEEK_CUR);
            last_auds = false;
        } else {
            fseek(g_vid_fp, (long)padded, SEEK_CUR);
        }
    }
}

// Read one 'movi' chunk: decode+show video frames, queue audio. false on EOF.
static bool avi_read_chunk(void)
{
    char cc[4];
    if (fread(cc, 1, 4, g_vid_fp) != 4) return false;
    uint32_t sz = favi_u32();
    uint32_t padded = sz + (sz & 1);

    if (cc[2] == 'd' && (cc[3] == 'c' || cc[3] == 'b')) {        // video frame
        uint32_t n = sz < TEXT_BUF_MAX ? sz : TEXT_BUF_MAX;
        if (fread(text_buf, 1, n, g_vid_fp) != n) return false;
        if (padded > n) fseek(g_vid_fp, (long)(padded - n), SEEK_CUR);

        lcd_clear_active_buffer();
        uint32_t w = 0, h = 0;
        if (JPEG_DecodeGetSize((uint32_t)text_buf, &w, &h) == 0 &&
            w > 0 && h > 0 && w <= GW_LCD_WIDTH && h <= GW_LCD_HEIGHT) {
            uint16_t x = (GW_LCD_WIDTH - w) / 2;
            uint16_t y = (GW_LCD_HEIGHT - h) / 2;
            JPEG_DecodeToFrame((uint32_t)text_buf, (uint32_t)lcd_get_active_buffer(), x, y, 255);
        }
        lcd_swap();
    } else if (cc[2] == 'w' && cc[3] == 'b') {                   // audio data
        uint32_t left = sz;
        while (left > 0) {
            uint32_t want = left < sizeof(g_apcm) ? left : sizeof(g_apcm);
            uint32_t got = (uint32_t)fread(g_apcm, 1, want, g_vid_fp);
            if (got == 0) break;
            int16_t *s16 = (int16_t *)g_apcm;
            int nsamp = (int)(got / 2);
            int m;
            if (g_va_channels >= 2) {
                m = nsamp / 2;
                for (int i = 0; i < m; i++)
                    g_amono[i] = (int16_t)(((int)s16[2*i] + s16[2*i+1]) / 2);
            } else {
                m = nsamp < 2048 ? nsamp : 2048;
                for (int i = 0; i < m; i++) g_amono[i] = s16[i];
            }
            va_push(g_amono, m);
            left -= got;
        }
        if (padded > sz) fseek(g_vid_fp, (long)(padded - sz), SEEK_CUR);
    } else {
        fseek(g_vid_fp, (long)padded, SEEK_CUR);
    }
    return true;
}

static void view_avi(const char *path)
{
    if (!avi_open(path)) {
        if (g_vid_fp) { fclose(g_vid_fp); g_vid_fp = NULL; }
        draw_center_msg("not a valid AVI");
        wait_for_b();
        return;
    }

    g_va_step = ((uint32_t)g_va_rate << 16) / AUDIO_SAMPLE_RATE;
    if (g_va_step == 0) g_va_step = 1;
    g_va_phase = 0;
    g_vr_head = g_vr_tail = g_vr_count = 0;

    JPEG_DecodeToFrameInit((uint32_t)g_scratch, SCRATCH_MAX);
    common_emu_state.skip_frames = 0;
    common_emu_state.pause_frames = 0;
    audio_start_playing(AUDIO_BUFFER_LENGTH);

    bool paused = false, done = false;
    odroid_gamepad_state_t joy, prev;
    odroid_input_read_gamepad(&prev);

    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&joy);
        if (joy.values[ODROID_INPUT_B] && !prev.values[ODROID_INPUT_B]) break;
        if (joy.values[ODROID_INPUT_A] && !prev.values[ODROID_INPUT_A]) paused = !paused;
        prev = joy;

        int16_t *buf = audio_get_active_buffer();
        int len = audio_get_buffer_length();
        int32_t vol = common_emu_sound_get_volume();
        for (int i = 0; i < len; i++) {
            int16_t s = (paused || g_vr_count == 0) ? 0 : vring_pull();
            buf[i] = (int16_t)((s * vol) >> 8);
        }

        if (!paused) {
            while (g_vr_count < AUDIO_BUFFER_LENGTH * 2 && ftell(g_vid_fp) < g_movi_end) {
                if (!avi_read_chunk()) { done = true; break; }
            }
        }

        common_emu_sound_sync(false);

        if (g_vr_count == 0 && (done || ftell(g_vid_fp) >= g_movi_end))
            break;
    }

    JPEG_DecodeDeInit();
    audio_stop_playing();
    if (g_vid_fp) { fclose(g_vid_fp); g_vid_fp = NULL; }
}

void app_main_media(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)load_state;
    (void)start_paused;
    (void)save_slot;

    odroid_system_init(APPID_HOMEBREW, 48000);

    strcpy(cur_path, MEDIA_ROOT);
    scan_current();

    odroid_gamepad_state_t joy, prev;
    memset(&prev, 0, sizeof(prev));

    lcd_clear_buffers();

    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&joy);

        #define PRESSED(btn) (joy.values[btn] && !prev.values[btn])

        if (PRESSED(ODROID_INPUT_UP))    move_cursor(-1);
        if (PRESSED(ODROID_INPUT_DOWN))  move_cursor(1);
        if (PRESSED(ODROID_INPUT_LEFT))  move_cursor(-VISIBLE_ROWS);
        if (PRESSED(ODROID_INPUT_RIGHT)) move_cursor(VISIBLE_ROWS);

        if (PRESSED(ODROID_INPUT_A)) {
            if (entry_count > 0) {
                media_entry_t *e = &entries[cursor];
                if (e->is_dir) {
                    enter_dir(e->name);
                } else {
                    char path[PATH_MAX_LEN];
                    if (strcmp(cur_path, "/") == 0)
                        snprintf(path, sizeof(path), "/%s", e->name);
                    else
                        snprintf(path, sizeof(path), "%s/%s", cur_path, e->name);

                    bool opened = true;
                    if (has_ext(e->name, ".txt"))
                        view_text(path);
                    else if (has_ext(e->name, ".jpg") || has_ext(e->name, ".jpeg") ||
                             has_ext(e->name, ".png"))
                        view_image(path);
                    else if (has_ext(e->name, ".mp3"))
                        view_mp3(path);
                    else if (has_ext(e->name, ".avi"))
                        view_avi(path);
                    else
                        opened = false;   // unsupported type (more in later increments)

                    if (opened) {
                        // resync input so a button still held on exit isn't re-fired here
                        odroid_input_read_gamepad(&joy);
                        prev = joy;
                        continue;
                    }
                }
            }
        }

        if (PRESSED(ODROID_INPUT_B)) {
            if (!go_parent())
                odroid_system_switch_app(APPID_LAUNCHER); // noreturn
        }

        #undef PRESSED

        prev = joy;

        draw();
        lcd_swap();
        lcd_wait_for_vblank();
    }
}
