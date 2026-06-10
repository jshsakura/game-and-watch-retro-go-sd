// Album-art rendering — see media_cover.h.

#include "media_cover.h"
#include "media_id3.h"
#include "gw_lcd.h"
#include "tjpgd.h"
#include "lupng.h"
#include <stdio.h>
#include <string.h>

#define IMG_BUF_MAX   (128 * 1024)   // raw cover bytes (JPEG/PNG)
#define SCRATCH_MAX   (352 * 1024)   // PNG decode pool

static uint8_t g_img[IMG_BUF_MAX];
static uint8_t g_scratch[SCRATCH_MAX];

// --- sidecar lookup ---------------------------------------------------------

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// Find a cover image beside the track (same-name .jpg/.png, or cover/folder.*).
static bool find_sidecar(const char *mp3path, char *out, size_t outsz, bool *is_png)
{
    char base[260];
    snprintf(base, sizeof(base), "%s", mp3path);
    char *dot = strrchr(base, '.');
    if (dot) {
        *dot = '\0';
        snprintf(out, outsz, "%s.jpg", base);
        if (file_exists(out)) { *is_png = false; return true; }
        snprintf(out, outsz, "%s.png", base);
        if (file_exists(out)) { *is_png = true; return true; }
    }
    char dir[260];
    snprintf(dir, sizeof(dir), "%s", mp3path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0'; else dir[0] = '\0';

    static const char *names[] = { "cover.jpg", "folder.jpg", "cover.png", "folder.png" };
    for (int i = 0; i < 4; i++) {
        snprintf(out, outsz, "%s/%s", dir, names[i]);
        if (file_exists(out)) { *is_png = (strstr(names[i], ".png") != NULL); return true; }
    }
    return false;
}

static int load_file(const char *path, uint8_t *dst, int cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int n = (int)fread(dst, 1, cap, f);
    fclose(f);
    return n < 0 ? 0 : n;
}

int cover_load(const char *path, bool *is_png)
{
    int n = id3_read_cover(path, g_img, IMG_BUF_MAX, is_png);
    if (n > 0) return n;

    char side[260];
    if (find_sidecar(path, side, sizeof(side), is_png))
        return load_file(side, g_img, IMG_BUF_MAX);
    return 0;
}

// --- JPEG (TJpgDec) ---------------------------------------------------------

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

static int g_ox, g_oy, g_clip_x0, g_clip_y0, g_clip_x1, g_clip_y1;

static int jpg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void)jd;
    uint16_t *fb = lcd_get_active_buffer();
    const uint16_t *src = (const uint16_t *)bitmap;
    for (int y = rect->top; y <= rect->bottom; y++) {
        int dy = g_oy + y;
        for (int x = rect->left; x <= rect->right; x++) {
            uint16_t px = *src++;
            int dx = g_ox + x;
            if (dx >= g_clip_x0 && dx < g_clip_x1 && dy >= g_clip_y0 && dy < g_clip_y1)
                fb[dy * GW_LCD_WIDTH + dx] = px;
        }
    }
    return 1;
}

// Decode g_img (n bytes), scaled to fit (bw,bh), centered in the box at (bx,by).
static bool jpeg_to_box(int n, int bx, int by, int bw, int bh)
{
    static uint8_t pool[8 * 1024];
    JDEC jd;
    jpg_src_t src = { g_img, (size_t)n, 0 };
    if (jd_prepare(&jd, jpg_in, pool, sizeof(pool), &src) != JDR_OK)
        return false;
    uint8_t sc = 0;
    while (sc < 3 && (((int)jd.width >> sc) > bw || ((int)jd.height >> sc) > bh))
        sc++;
    int w = (int)jd.width >> sc, h = (int)jd.height >> sc;
    g_ox = bx + (bw - w) / 2;
    g_oy = by + (bh - h) / 2;
    g_clip_x0 = bx; g_clip_y0 = by;
    g_clip_x1 = bx + bw; g_clip_y1 = by + bh;
    if (g_clip_x1 > GW_LCD_WIDTH)  g_clip_x1 = GW_LCD_WIDTH;
    if (g_clip_y1 > GW_LCD_HEIGHT) g_clip_y1 = GW_LCD_HEIGHT;
    return jd_decomp(&jd, jpg_out, sc) == JDR_OK;
}

// --- PNG (lupng over a bump allocator) --------------------------------------

static size_t g_scratch_off;

static void *scratch_alloc(size_t size, void *u)
{
    (void)u;
    size = (size + 3u) & ~(size_t)3u;
    if (g_scratch_off + size > SCRATCH_MAX) return NULL;
    void *p = g_scratch + g_scratch_off;
    g_scratch_off += size;
    return p;
}
static void scratch_free(void *p, void *u) { (void)p; (void)u; }

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

// Blit an 8-bit RGB(A) image into the box (bx,by,bw,bh), centered + downscaled.
static void blit_rgb_box(const uint8_t *px, int w, int h, int ch,
                         int bx, int by, int bw, int bh)
{
    uint16_t *fb = lcd_get_active_buffer();
    int tw = w, th = h;
    if (tw > bw) { th = th * bw / tw; tw = bw; }
    if (th > bh) { tw = tw * bh / th; th = bh; }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    int ox = bx + (bw - tw) / 2;
    int oy = by + (bh - th) / 2;

    for (int y = 0; y < th; y++) {
        int sy = y * h / th;
        const uint8_t *row = px + (size_t)sy * w * ch;
        int dy = oy + y;
        if (dy < 0 || dy >= GW_LCD_HEIGHT) continue;
        uint16_t *dst = fb + (size_t)dy * GW_LCD_WIDTH + ox;
        for (int x = 0; x < tw; x++) {
            int dx = ox + x;
            if (dx < 0 || dx >= GW_LCD_WIDTH) continue;
            const uint8_t *p = row + (size_t)(x * w / tw) * ch;
            dst[x] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        }
    }
}

static bool png_to_box(int n, int bx, int by, int bw, int bh)
{
    mem_reader_t rd = { g_img, (size_t)n, 0 };
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
    blit_rgb_box(img->data, img->width, img->height, img->channels, bx, by, bw, bh);
    return true;
}

// --- public render ----------------------------------------------------------

static void darken_all(void)
{
    uint16_t *fb = lcd_get_active_buffer();
    int n = GW_LCD_WIDTH * GW_LCD_HEIGHT;
    for (int i = 0; i < n; i++) {
        uint16_t c = fb[i];
        c = (uint16_t)((c >> 1) & 0x7BEF);     // 1/2
        c = (uint16_t)((c >> 1) & 0x7BEF);     // 1/4
        fb[i] = c;
    }
}

bool cover_render_backdrop(int n, bool is_png)
{
    if (n <= 0) return false;
    bool ok = is_png ? png_to_box(n, 0, 0, GW_LCD_WIDTH, GW_LCD_HEIGHT)
                     : jpeg_to_box(n, 0, 0, GW_LCD_WIDTH, GW_LCD_HEIGHT);
    if (ok) darken_all();
    return ok;
}

bool cover_render_card(int n, bool is_png, int bx, int by, int bw, int bh)
{
    if (n <= 0) return false;
    return is_png ? png_to_box(n, bx, by, bw, bh)
                  : jpeg_to_box(n, bx, by, bw, bh);
}

// --- thumbnail (JPEG only) --------------------------------------------------

static uint16_t *g_thumb_dst;
static int g_thumb_sw, g_thumb_sh, g_thumb_sz;

static int jpg_thumb_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void)jd;
    const uint16_t *src = (const uint16_t *)bitmap;
    for (int y = rect->top; y <= rect->bottom; y++)
        for (int x = rect->left; x <= rect->right; x++) {
            uint16_t px = *src++;
            int tx = g_thumb_sw > 0 ? x * g_thumb_sz / g_thumb_sw : 0;
            int ty = g_thumb_sh > 0 ? y * g_thumb_sz / g_thumb_sh : 0;
            if (tx < g_thumb_sz && ty < g_thumb_sz) g_thumb_dst[ty * g_thumb_sz + tx] = px;
        }
    return 1;
}

static bool jpeg_to_thumb(int n, uint16_t *out, int sz)
{
    static uint8_t pool[8 * 1024];
    JDEC jd;
    jpg_src_t src = { g_img, (size_t)n, 0 };
    if (jd_prepare(&jd, jpg_in, pool, sizeof(pool), &src) != JDR_OK)
        return false;
    uint8_t sc = 0;
    while (sc < 3 && (((int)jd.width >> sc) > sz * 4 || ((int)jd.height >> sc) > sz * 4)) sc++;
    g_thumb_sw = (int)jd.width >> sc;
    g_thumb_sh = (int)jd.height >> sc;
    g_thumb_dst = out;
    g_thumb_sz = sz;
    memset(out, 0, (size_t)sz * sz * sizeof(uint16_t));
    return jd_decomp(&jd, jpg_thumb_out, sc) == JDR_OK;
}

bool cover_thumb(const char *path, uint16_t *out, int sz)
{
    bool is_png = false;
    int n = cover_load(path, &is_png);
    if (n <= 0 || is_png) return false;        // PNG thumbnails skipped
    return jpeg_to_thumb(n, out, sz);
}
