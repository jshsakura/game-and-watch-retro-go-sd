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
#include "tjpgd.h"
#include "minimp3.h"
#include "gw_audio.h"
#include "common.h"
#include "gui.h"
#include "main_media.h"

// Localized strings with English fallback (in case a language lacks the key).
#define MEDIA_HINT  ((curr_lang && curr_lang->s_media_hint)  ? curr_lang->s_media_hint  : "A:Open  B:Back")
#define MEDIA_EMPTY ((curr_lang && curr_lang->s_media_empty) ? curr_lang->s_media_empty : "(empty)")

#define MEDIA_ROOT      "/media"
#define MAX_ENTRIES     256
#define NAME_MAX_LEN    128
#define PATH_MAX_LEN    256

#define ROW_HEIGHT      34
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
#define TEXT_BUF_MAX    (128 * 1024)  // max file bytes loaded (truncated beyond)
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
static char g_root[PATH_MAX_LEN] = "/music";  // chosen at startup (/music or /media)

// Rich list: per-track metadata (album-art thumbnail + duration), cached and
// decoded lazily for the visible rows.
#define THUMB_SZ  28
#define META_N    16
typedef struct {
    int  idx;                            // entry index this slot describes
    bool valid, has_art;
    int  dur;                            // seconds (0 = unknown)
    uint16_t art[THUMB_SZ * THUMB_SZ];   // RGB565 thumbnail
} track_meta_t;
static track_meta_t g_meta[META_N];
static int g_meta_clock;

static bool has_ext(const char *name, const char *ext);              // defined later
static void fill_rect(int x, int y, int w, int h, uint16_t c);       // defined later
static const track_meta_t *meta_get(int entry_idx);                 // defined later

static int scandir_cb(const rg_scandir_t *file, void *arg)
{
    (void)arg;
    if (entry_count >= MAX_ENTRIES)
        return RG_SCANDIR_STOP;

    // music browser: only folders and .mp3 files
    if (!file->is_dir && !has_ext(file->basename, ".mp3"))
        return RG_SCANDIR_CONTINUE;

    media_entry_t *e = &entries[entry_count++];
    strncpy(e->name, file->basename, NAME_MAX_LEN - 1);
    e->name[NAME_MAX_LEN - 1] = '\0';
    e->is_dir = file->is_dir;
    return RG_SCANDIR_CONTINUE;
}

static bool scan_current(void)
{
    entry_count = 0;
    cursor = 0;
    scroll = 0;
    for (int i = 0; i < META_N; i++) g_meta[i].valid = false;   // folder changed
    return rg_storage_scandir(cur_path, scandir_cb, NULL,
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
    if (strcmp(cur_path, g_root) == 0)
        return false;

    char *slash = strrchr(cur_path, '/');
    if (!slash || slash == cur_path)
        strcpy(cur_path, g_root);
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

// All on-screen text goes through i18n so CJK (Korean) renders correctly; the
// plain odroid_overlay_draw_text uses an ASCII-only 8x8 font.
static int draw_text(uint16_t x, uint16_t y, uint16_t w, const char *t, uint16_t color, uint16_t bg)
{
    return i18n_draw_text_line(x, y, w, t, color, bg, 0);
}

static void draw(void)
{
    uint16_t bgc = curr_colors->bg_c, fgc = curr_colors->main_c;
    uint16_t selc = curr_colors->sel_c, dimc = curr_colors->dis_c;

    fill_rect(0, 0, GW_LCD_WIDTH, GW_LCD_HEIGHT, bgc);

    // header: current folder + divider
    draw_text(6, 3, GW_LCD_WIDTH - 12, cur_path, selc, bgc);
    fill_rect(0, HEADER_HEIGHT - 1, GW_LCD_WIDTH, 1, dimc);

    if (entry_count == 0)
        draw_text(6, LIST_TOP + 10, GW_LCD_WIDTH - 12, MEDIA_EMPTY, fgc, bgc);

    for (int row = 0; row < VISIBLE_ROWS; row++) {
        int idx = scroll + row;
        if (idx >= entry_count) break;

        media_entry_t *e = &entries[idx];
        int y = LIST_TOP + row * ROW_HEIGHT;
        bool sel = (idx == cursor);
        uint16_t rbg = sel ? selc : bgc;
        uint16_t txt = sel ? bgc  : fgc;
        uint16_t sub = sel ? bgc  : dimc;
        if (sel) fill_rect(0, y, GW_LCD_WIDTH, ROW_HEIGHT, selc);

        int tx = 6, ty = y + (ROW_HEIGHT - THUMB_SZ) / 2;

        if (e->is_dir) {
            fill_rect(tx, ty + 4, THUMB_SZ, THUMB_SZ - 8, sub);   // folder glyph
            draw_text(tx + THUMB_SZ + 8, y + 10,
                GW_LCD_WIDTH - (tx + THUMB_SZ + 8) - 8, e->name, txt, rbg);
        } else {
            const track_meta_t *m = meta_get(idx);

            char t[NAME_MAX_LEN];
            snprintf(t, sizeof(t), "%s", e->name);
            char *dot = strrchr(t, '.'); if (dot) *dot = '\0';
            draw_text(tx + THUMB_SZ + 8, y + 5,
                GW_LCD_WIDTH - (tx + THUMB_SZ + 8) - 52, t, txt, rbg);

            char d[16];
            if (m->dur > 0) snprintf(d, sizeof(d), "%d:%02d", m->dur / 60, m->dur % 60);
            else            snprintf(d, sizeof(d), "--:--");
            draw_text(GW_LCD_WIDTH - 48, y + 18, 44, d, sub, rbg);

            uint16_t *fb = lcd_get_active_buffer();   // thumbnail last (on top)
            if (m->has_art) {
                for (int j = 0; j < THUMB_SZ; j++)
                    for (int i = 0; i < THUMB_SZ; i++)
                        fb[(ty + j) * GW_LCD_WIDTH + (tx + i)] = m->art[j * THUMB_SZ + i];
            } else {
                fill_rect(tx, ty, THUMB_SZ, THUMB_SZ, sub);
            }
        }
    }

    fill_rect(0, GW_LCD_HEIGHT - FOOTER_HEIGHT, GW_LCD_WIDTH, FOOTER_HEIGHT, bgc);
    draw_text(6, GW_LCD_HEIGHT - FOOTER_HEIGHT + 1, GW_LCD_WIDTH - 12,
        "A:Play   <>:Track   B:Back", dimc, bgc);
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
        draw_text(4, 2 + r * ROW_HEIGHT, GW_LCD_WIDTH - 8,
            line, COLOR_TEXT, COLOR_BG);
    }

    int consumed = (int)(p - base);
    int pct = text_len ? (int)((long)(start + consumed) * 100 / text_len) : 100;
    char foot[32];
    snprintf(foot, sizeof(foot), "B   %d%%", pct);
    draw_text(4, GW_LCD_HEIGHT - FOOTER_HEIGHT + 2, GW_LCD_WIDTH - 8,
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

#define SCRATCH_MAX     (352 * 1024)  // JPEG work buffer / PNG decode pool
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
    draw_text(4, GW_LCD_HEIGHT / 2 - 6, GW_LCD_WIDTH - 8,
        msg, COLOR_TEXT, COLOR_BG);
    draw_text(4, GW_LCD_HEIGHT - FOOTER_HEIGHT + 2, GW_LCD_WIDTH - 8,
        "B", COLOR_HEADER, COLOR_BG);
    lcd_swap();
}

// Decode a JPEG already in text_buf (n bytes) to the active framebuffer (no
// swap). Returns false (drawing nothing) if it isn't a decodable, screen-sized
// JPEG. Large JPEGs can't be downscaled on this hardware (work + output buffers
// scale with image size and overrun RAM).
static bool show_jpeg_buf(int n)
{
    if (n <= 0) return false;

    JPEG_DecodeToFrameInit((uint32_t)g_scratch, SCRATCH_MAX);
    uint32_t w = 0, h = 0;
    if (JPEG_DecodeGetSize((uint32_t)text_buf, &w, &h) != 0) {
        JPEG_DecodeDeInit();
        return false;
    }
    if (w == 0 || h == 0 || w > GW_LCD_WIDTH || h > GW_LCD_HEIGHT) {
        JPEG_DecodeDeInit();
        return false;
    }

    lcd_clear_active_buffer();
    uint16_t x = (GW_LCD_WIDTH - w) / 2;
    uint16_t y = (GW_LCD_HEIGHT - h) / 2;
    JPEG_DecodeToFrame((uint32_t)text_buf, (uint32_t)lcd_get_active_buffer(), x, y, 255);
    JPEG_DecodeDeInit();
    return true;
}

static void show_jpeg(const char *path)
{
    int n = load_file(path, TEXT_BUF_MAX);
    if (!show_jpeg_buf(n)) {
        draw_center_msg("이미지를 열 수 없음 (너무 크거나 손상)");
        return;
    }
    draw_text(4, GW_LCD_HEIGHT - FOOTER_HEIGHT + 2, GW_LCD_WIDTH - 8,
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

    // scale down to fit the screen, keeping aspect ratio (no upscaling)
    int tw = w, th = h;
    if (tw > GW_LCD_WIDTH)  { th = th * GW_LCD_WIDTH  / tw; tw = GW_LCD_WIDTH;  }
    if (th > GW_LCD_HEIGHT) { tw = tw * GW_LCD_HEIGHT / th; th = GW_LCD_HEIGHT; }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    int ox = (GW_LCD_WIDTH  - tw) / 2;
    int oy = (GW_LCD_HEIGHT - th) / 2;

    for (int y = 0; y < th; y++) {
        int sy = y * h / th;
        const uint8_t *row = px + (size_t)sy * w * ch;
        uint16_t *dst = fb + (size_t)(oy + y) * GW_LCD_WIDTH + ox;
        for (int x = 0; x < tw; x++) {
            int sx = x * w / tw;
            const uint8_t *p = row + (size_t)sx * ch;
            dst[x] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        }
    }
}

// Decode a PNG already in text_buf (n bytes) to the active framebuffer (no
// swap, downscaled to fit). Returns false if it can't be decoded.
static bool show_png_buf(int n)
{
    if (n <= 0) return false;

    mem_reader_t rd = { (const uint8_t *)text_buf, (size_t)n, 0 };
    g_scratch_off = 0;

    LuUserContext uc;
    luUserContextInitDefault(&uc);
    uc.readProc = mem_read;       uc.readProcUserPtr = &rd;
    uc.allocProc = scratch_alloc; uc.allocProcUserPtr = NULL;
    uc.freeProc = scratch_free;   uc.freeProcUserPtr = NULL;
    uc.warnProc = NULL;

    LuImage *img = luPngReadUC(&uc);
    if (!img) return false;
    if (img->depth != 8 || img->channels < 3) return false;

    blit_rgb_centered(img->data, img->width, img->height, img->channels);
    return true;
}

static void show_png(const char *path)
{
    int n = load_file(path, TEXT_BUF_MAX);
    if (!show_png_buf(n)) {
        draw_center_msg("PNG을 열 수 없음 (너무 크거나 미지원)");
        return;
    }
    draw_text(4, GW_LCD_HEIGHT - FOOTER_HEIGHT + 2, GW_LCD_WIDTH - 8,
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
static int       g_mp3_bitrate;   // kbps of the last decoded frame

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
            if (info.bitrate_kbps > 0)
                g_mp3_bitrate = info.bitrate_kbps;
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

// Decoded-PCM ring (48 kHz mono), shared with the AVI player. Decoupling the SD
// read + decode from the audio-buffer fill prevents the periodic (~1/s) stutter
// caused by a large MP3 input refill landing on an audio deadline.
#define VRING_SIZE  8192            // must be a power of two
#define VRING_MASK  (VRING_SIZE - 1)
static int16_t g_vring[VRING_SIZE];
static int     g_vr_head, g_vr_tail, g_vr_count;

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

// Decode + resample ahead into the ring until it holds >= target samples.
static void mp3_pump(int target)
{
    while (g_vr_count < target && !g_mp3_eof) {
        while ((g_mp3_phase >> 16) >= (uint32_t)g_mp3_frame_n) {
            g_mp3_phase -= (uint32_t)g_mp3_frame_n << 16;
            if (!mp3_decode_frame()) { g_mp3_eof = true; break; }
        }
        if (g_mp3_eof) break;
        vring_push(g_mp3_mono[g_mp3_phase >> 16]);
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

// Read the ID3v2 APIC frame (embedded album art) into text_buf. Returns the
// number of image bytes loaded (0 if none) and sets *is_png_out.
static int extract_id3_art(const char *path, bool *is_png_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint8_t hdr[10];
    if (fread(hdr, 1, 10, f) != 10 || memcmp(hdr, "ID3", 3) != 0) { fclose(f); return false; }
    int ver = hdr[3];   // 3 = ID3v2.3, 4 = ID3v2.4
    uint32_t tagsize = ((uint32_t)(hdr[6] & 0x7f) << 21) | ((uint32_t)(hdr[7] & 0x7f) << 14) |
                       ((uint32_t)(hdr[8] & 0x7f) << 7) | (uint32_t)(hdr[9] & 0x7f);
    long tag_end = 10 + (long)tagsize;

    bool found = false, is_png = false;
    int img_n = 0;
    long pos = 10;

    while (pos + 10 <= tag_end) {
        if (fseek(f, pos, SEEK_SET) != 0) break;
        uint8_t fh[10];
        if (fread(fh, 1, 10, f) != 10) break;
        if (fh[0] == 0) break;   // padding region

        uint32_t fsize = (ver >= 4)
            ? (((uint32_t)(fh[4] & 0x7f) << 21) | ((uint32_t)(fh[5] & 0x7f) << 14) |
               ((uint32_t)(fh[6] & 0x7f) << 7) | (uint32_t)(fh[7] & 0x7f))
            : (((uint32_t)fh[4] << 24) | ((uint32_t)fh[5] << 16) |
               ((uint32_t)fh[6] << 8) | (uint32_t)fh[7]);
        long fdata = pos + 10;

        if (memcmp(fh, "APIC", 4) == 0 && fsize > 10 && fsize < 8u * 1024 * 1024) {
            fseek(f, fdata, SEEK_SET);
            long consumed = 0;
            int enc = fgetc(f); consumed++;                          // text encoding
            while (consumed < (long)fsize && fgetc(f) != 0) consumed++;  // MIME string
            consumed++;                                              // MIME null
            (void)fgetc(f); consumed++;                              // picture type
            if (enc == 1 || enc == 2) {                              // UTF-16 desc: double null
                int z = 0;
                while (consumed < (long)fsize) { int c = fgetc(f); consumed++; if (c == 0) { if (++z == 2) break; } else z = 0; }
            } else {                                                 // latin1/utf8 desc: single null
                while (consumed < (long)fsize && fgetc(f) != 0) consumed++;
                consumed++;
            }
            long isz = (long)fsize - consumed;
            if (isz > 8) {
                long lim = isz < (long)TEXT_BUF_MAX ? isz : (long)TEXT_BUF_MAX;
                img_n = (int)fread(text_buf, 1, (size_t)lim, f);
                if (img_n > 8) {
                    uint8_t *b = (uint8_t *)text_buf;
                    is_png = (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G');
                    found = true;
                }
            }
            break;   // first APIC only
        }
        pos = fdata + (long)fsize;
    }
    fclose(f);
    if (!found) return 0;
    *is_png_out = is_png;
    return img_n;
}

// ===========================================================================
// Music player: iPod-style now-playing screen (cover + title + progress bar),
// sequential / shuffle playback, skip, pause, auto-advance to the next track.
// ===========================================================================

static uint32_t g_rng = 0x9e3779b9u;
static uint32_t rng(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static void fill_rect(int x, int y, int w, int h, uint16_t c)
{
    uint16_t *fb = lcd_get_active_buffer();
    for (int j = 0; j < h; j++) {
        int yy = y + j; if (yy < 0 || yy >= GW_LCD_HEIGHT) continue;
        uint16_t *row = fb + yy * GW_LCD_WIDTH;
        for (int i = 0; i < w; i++) { int xx = x + i; if (xx >= 0 && xx < GW_LCD_WIDTH) row[xx] = c; }
    }
}

// Decode the track's cover (embedded ID3, else sidecar) to the active buffer
// (no swap). Returns true if a cover was drawn.
// --- Scaled JPEG decode (TJpgDec): cover art of ANY size fits in ~8 KB of RAM
// by decoding at 1/1..1/8 scale, unlike the full-res hardware decoder. ---
typedef struct { const uint8_t *data; size_t size, pos; } jpg_src_t;

static size_t jpg_in(JDEC *jd, uint8_t *buf, size_t len)
{
    jpg_src_t *s = (jpg_src_t *)jd->device;
    size_t avail = s->size - s->pos;
    if (len > avail) len = avail;
    if (buf) memcpy(buf, s->data + s->pos, len);
    s->pos += len;
    return len;
}

static int g_jpg_ox, g_jpg_oy;
static int jpg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void)jd;
    uint16_t *fb = lcd_get_active_buffer();
    const uint16_t *src = (const uint16_t *)bitmap;
    for (int y = rect->top; y <= rect->bottom; y++) {
        int dy = g_jpg_oy + y;
        for (int x = rect->left; x <= rect->right; x++) {
            uint16_t px = *src++;
            int dx = g_jpg_ox + x;
            if ((unsigned)dx < (unsigned)GW_LCD_WIDTH && (unsigned)dy < (unsigned)GW_LCD_HEIGHT)
                fb[dy * GW_LCD_WIDTH + dx] = px;
        }
    }
    return 1;
}

// Decode the JPEG in text_buf (n bytes), down-scaled to fit and centered, to the
// active buffer (no swap). Returns true on success.
static bool tjpgd_cover(int n)
{
    static uint8_t pool[8 * 1024];
    JDEC jd;
    jpg_src_t src = { (const uint8_t *)text_buf, (size_t)n, 0 };
    if (jd_prepare(&jd, jpg_in, pool, sizeof(pool), &src) != JDR_OK)
        return false;
    uint8_t sc = 0;
    while (sc < 3 && ((jd.width >> sc) > GW_LCD_WIDTH || (jd.height >> sc) > GW_LCD_HEIGHT))
        sc++;
    g_jpg_ox = (GW_LCD_WIDTH  - (int)(jd.width  >> sc)) / 2;
    g_jpg_oy = (GW_LCD_HEIGHT - (int)(jd.height >> sc)) / 2;
    lcd_clear_active_buffer();
    return jd_decomp(&jd, jpg_out, sc) == JDR_OK;
}

static bool cover_to_active(const char *path)
{
    bool is_png = false;
    int n = extract_id3_art(path, &is_png);
    if (n > 0 && (is_png ? show_png_buf(n) : tjpgd_cover(n)))
        return true;

    char cover[PATH_MAX_LEN];
    if (find_cover(path, cover, sizeof(cover), &is_png)) {
        n = load_file(cover, TEXT_BUF_MAX);
        if (n > 0 && (is_png ? show_png_buf(n) : tjpgd_cover(n)))
            return true;
    }
    return false;
}

// Draw the static background (cover, or a clean placeholder) into BOTH buffers.
static void draw_player_bg(const char *path)
{
    for (int i = 0; i < 2; i++) {
        if (!cover_to_active(path))
            fill_rect(0, 0, GW_LCD_WIDTH, GW_LCD_HEIGHT, curr_colors->bg_c);  // themed bg
        lcd_swap();
    }
}

// Now-playing panel over the cover, drawn in the current UI theme colours:
// title, progress bar with elapsed/remaining time, and the shuffle indicator.
static void draw_now_playing(const char *name, int sec, int total, bool paused, bool shuffle)
{
    uint16_t bg = curr_colors->bg_c, fg = curr_colors->main_c;
    uint16_t acc = curr_colors->sel_c, dim = curr_colors->dis_c;

    int ph = 46, py = GW_LCD_HEIGHT - ph;
    fill_rect(0, py, GW_LCD_WIDTH, ph, bg);          // themed panel
    fill_rect(0, py, GW_LCD_WIDTH, 1, acc);          // accent line

    char line[160];
    snprintf(line, sizeof(line), "%s%s", paused ? "II " : "> ", name);
    draw_text(8, py + 4, GW_LCD_WIDTH - 16, line, fg, bg);

    int bx = 8, bw = GW_LCD_WIDTH - 16, by = py + 22, bh = 4;
    fill_rect(bx, by, bw, bh, dim);                  // track
    int frac = total > 0 ? sec * bw / total : 0;
    if (frac > bw) frac = bw;
    if (frac < 0) frac = 0;
    fill_rect(bx, by, frac, bh, acc);                // elapsed
    fill_rect(bx + frac - 1, by - 2, 3, bh + 4, fg); // knob

    char t[40];
    int rem = total > sec ? total - sec : 0;
    snprintf(t, sizeof(t), "%d:%02d", sec / 60, sec % 60);
    draw_text(8, py + 30, 64, t, dim, bg);
    snprintf(t, sizeof(t), "-%d:%02d", rem / 60, rem % 60);
    draw_text(GW_LCD_WIDTH - 60, py + 30, 56, t, dim, bg);
    if (shuffle)
        draw_text(GW_LCD_WIDTH / 2 - 28, py + 30, 70, "SHUFFLE", acc, bg);

    lcd_swap();
}

static int find_mp3(int from, int dir)
{
    for (int i = from; i >= 0 && i < entry_count; i += dir)
        if (!entries[i].is_dir && has_ext(entries[i].name, ".mp3")) return i;
    return -1;
}
static int count_mp3(void)
{
    int c = 0;
    for (int i = 0; i < entry_count; i++)
        if (!entries[i].is_dir && has_ext(entries[i].name, ".mp3")) c++;
    return c;
}
static int nth_mp3(int n)
{
    for (int i = 0; i < entry_count; i++)
        if (!entries[i].is_dir && has_ext(entries[i].name, ".mp3"))
            if (n-- == 0) return i;
    return -1;
}
static int pick_next(int cur, bool shuffle)
{
    if (shuffle) { int c = count_mp3(); return c > 0 ? nth_mp3((int)(rng() % (uint32_t)c)) : -1; }
    return find_mp3(cur + 1, 1);
}

static void track_path(char *out, size_t outsz, int idx)
{
    if (strcmp(cur_path, "/") == 0) snprintf(out, outsz, "/%s", entries[idx].name);
    else snprintf(out, outsz, "%s/%s", cur_path, entries[idx].name);
}

static void track_open(const char *path)
{
    if (g_mp3_fp) fclose(g_mp3_fp);
    g_mp3_fp = fopen(path, "rb");
    mp3dec_init(&g_mp3);
    g_mp3_in_len = g_mp3_in_pos = 0;
    g_mp3_file_eof = (g_mp3_fp == NULL);
    g_mp3_frame_n = 0;
    g_mp3_phase = 0;
    g_mp3_step = ((uint32_t)44100 << 16) / AUDIO_SAMPLE_RATE;
    g_mp3_eof = false;
    g_mp3_bitrate = 0;
}

static int estimate_total_sec(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    if (g_mp3_bitrate <= 0 || sz <= 0) return 0;
    return (int)((long long)sz * 8 / ((long long)g_mp3_bitrate * 1000));
}

static void music_player(int start_idx)
{
    g_rng ^= dma_counter + (uint32_t)start_idx + 1u;
    common_emu_state.skip_frames = 0;
    common_emu_state.pause_frames = 0;
    audio_start_playing(AUDIO_BUFFER_LENGTH);

    int idx = start_idx, total_sec = 0, last_sec = -1;
    bool reload = true, paused = false, shuffle = false;
    uint32_t played = 0;
    char path[PATH_MAX_LEN];
    const char *name = "";

    odroid_gamepad_state_t joy, prev;
    odroid_input_read_gamepad(&prev);

    while (true) {
        if (reload) {
            reload = false; paused = false; last_sec = -1; played = 0;
            track_path(path, sizeof(path), idx);
            const char *s = strrchr(path, '/'); name = s ? s + 1 : path;
            track_open(path);
            g_vr_head = g_vr_tail = g_vr_count = 0;
            mp3_pump(VRING_SIZE - 1152);
            total_sec = estimate_total_sec(path);
            draw_player_bg(path);
        }

        wdog_refresh();
        odroid_input_read_gamepad(&joy);
        #define P(b) (joy.values[b] && !prev.values[b])
        if (P(ODROID_INPUT_B)) break;
        if (P(ODROID_INPUT_A)) { paused = !paused; last_sec = -1; }
        if (P(ODROID_INPUT_UP)) { shuffle = !shuffle; last_sec = -1; }
        if (P(ODROID_INPUT_RIGHT)) { int n = pick_next(idx, shuffle); if (n >= 0) { idx = n; reload = true; } }
        if (P(ODROID_INPUT_LEFT))  { int n = find_mp3(idx - 1, -1);   if (n >= 0) { idx = n; reload = true; } }
        #undef P
        prev = joy;
        if (reload) continue;

        int16_t *buf = audio_get_active_buffer();
        int len = audio_get_buffer_length();
        int32_t vol = common_emu_sound_get_volume();
        for (int i = 0; i < len; i++) {
            int16_t sm = (paused || g_mp3_fp == NULL || g_vr_count == 0) ? 0 : vring_pull();
            buf[i] = (int16_t)((sm * vol) >> 8);
        }
        if (!paused) { played += len; mp3_pump(VRING_SIZE - 1152); }

        int sec = (int)(played / AUDIO_SAMPLE_RATE);
        if (sec != last_sec) { last_sec = sec; draw_now_playing(name, sec, total_sec, paused, shuffle); }

        common_emu_sound_sync(false);

        if (g_mp3_eof && g_vr_count == 0) {
            int n = pick_next(idx, shuffle);
            if (n >= 0) { idx = n; reload = true; }
            else break;
        }
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

static FILE     *g_vid_fp;
static long      g_movi_end;
static uint32_t  g_va_rate, g_va_step, g_va_phase;  // audio resample (in->48k)
static int       g_va_channels;
static uint8_t   g_apcm[4096];
static int16_t   g_amono[2048];

static uint32_t favi_u32(void)
{
    uint8_t b[4];
    if (fread(b, 1, 4, g_vid_fp) != 4) return 0;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
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

// --- album-art thumbnails: decode a cover JPEG (in text_buf) into a THUMB box ---
static uint16_t *g_thumb_dst;
static int g_thumb_sw, g_thumb_sh;

static int jpg_thumb_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void)jd;
    const uint16_t *src = (const uint16_t *)bitmap;
    for (int y = rect->top; y <= rect->bottom; y++)
        for (int x = rect->left; x <= rect->right; x++) {
            uint16_t px = *src++;
            int tx = g_thumb_sw > 0 ? x * THUMB_SZ / g_thumb_sw : 0;
            int ty = g_thumb_sh > 0 ? y * THUMB_SZ / g_thumb_sh : 0;
            if (tx < THUMB_SZ && ty < THUMB_SZ) g_thumb_dst[ty * THUMB_SZ + tx] = px;
        }
    return 1;
}

static bool jpeg_to_thumb(int n, uint16_t *out)
{
    static uint8_t pool[8 * 1024];
    JDEC jd;
    jpg_src_t src = { (const uint8_t *)text_buf, (size_t)n, 0 };
    if (jd_prepare(&jd, jpg_in, pool, sizeof(pool), &src) != JDR_OK)
        return false;
    uint8_t sc = 0;
    while (sc < 3 && ((jd.width >> sc) > THUMB_SZ * 4 || (jd.height >> sc) > THUMB_SZ * 4)) sc++;
    g_thumb_sw = jd.width >> sc; g_thumb_sh = jd.height >> sc; g_thumb_dst = out;
    memset(out, 0, THUMB_SZ * THUMB_SZ * sizeof(uint16_t));
    return jd_decomp(&jd, jpg_thumb_out, sc) == JDR_OK;
}

// Estimate track length (seconds) from the first frame's bitrate and file size.
static int read_duration(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t h[10];
    long off = 0;
    if (fread(h, 1, 10, f) == 10 && memcmp(h, "ID3", 3) == 0)
        off = 10 + (long)(((uint32_t)(h[6] & 0x7f) << 21) | ((uint32_t)(h[7] & 0x7f) << 14) |
                          ((uint32_t)(h[8] & 0x7f) << 7) | (uint32_t)(h[9] & 0x7f));
    fseek(f, off, SEEK_SET);
    int got = (int)fread(g_mp3_in, 1, 4096, f);    // reuse the player input buffer
    fclose(f);
    if (got <= 0) return 0;

    mp3dec_init(&g_mp3);
    mp3dec_frame_info_t info;
    int pos = 0, br = 0;
    while (pos < got) {
        int s = mp3dec_decode_frame(&g_mp3, g_mp3_in + pos, got - pos, g_mp3_pcm, &info);
        pos += info.frame_bytes;
        if (info.frame_bytes == 0) break;
        if (s > 0 && info.bitrate_kbps > 0) { br = info.bitrate_kbps; break; }
    }
    if (br <= 0) return 0;
    return (int)((long long)(sz - off) * 8 / ((long long)br * 1000));
}

// Lazily fetch (and cache) a track's thumbnail + duration.
static const track_meta_t *meta_get(int entry_idx)
{
    for (int i = 0; i < META_N; i++)
        if (g_meta[i].valid && g_meta[i].idx == entry_idx) return &g_meta[i];

    track_meta_t *m = &g_meta[g_meta_clock];
    g_meta_clock = (g_meta_clock + 1) % META_N;
    m->idx = entry_idx; m->valid = true; m->has_art = false; m->dur = 0;

    char path[PATH_MAX_LEN];
    track_path(path, sizeof(path), entry_idx);
    m->dur = read_duration(path);

    bool is_png = false;
    int n = extract_id3_art(path, &is_png);
    if (n > 0 && !is_png) {
        m->has_art = jpeg_to_thumb(n, m->art);
    } else {
        char cov[PATH_MAX_LEN];
        bool ip = false;
        if (find_cover(path, cov, sizeof(cov), &ip) && !ip) {
            n = load_file(cov, TEXT_BUF_MAX);
            if (n > 0) m->has_art = jpeg_to_thumb(n, m->art);
        }
    }
    return m;
}

void app_main_media(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)load_state;
    (void)start_paused;
    (void)save_slot;

    odroid_system_init(APPID_HOMEBREW, 48000);

    strcpy(cur_path, "/music");
    if (!scan_current()) {           // fall back to /media if /music is absent
        strcpy(cur_path, "/media");
        scan_current();
    }
    strcpy(g_root, cur_path);

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
                } else if (has_ext(e->name, ".mp3")) {
                    music_player(cursor);
                    // resync input so a button still held on exit isn't re-fired here
                    odroid_input_read_gamepad(&joy);
                    prev = joy;
                    continue;
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
