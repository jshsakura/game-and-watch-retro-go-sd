// Album-art rendering — see music_cover.h.

#include "music_cover.h"
#include "music_id3.h"
#include "gw_lcd.h"
#include "tjpgd.h"
#include "lupng.h"
#include "progjpeg.h"
#include <stdio.h>
#include <string.h>

#define SCRATCH_MAX   (352 * 1024)   // PNG inflate pool — also lent to JPEG as its
                                     // work area (see JPEG_WORK_SZ); the two decoders
                                     // never run at once, so they share this buffer.
#define JPEG_WORK_SZ  (32 * 1024)    // tjpgd work area, carved from g_scratch

static uint8_t g_scratch[SCRATCH_MAX];

_Static_assert(JPEG_WORK_SZ <= SCRATCH_MAX, "JPEG work area must fit in g_scratch");

// Cover art is decoded by STREAMING straight from the file (embedded APIC at an
// offset, or a sidecar starting at 0) — we never hold the whole compressed image
// in RAM, so arbitrarily large covers decode and there is no size cap. g_src is
// the located source; cover_open() returns a FILE* seeked to the image start.
typedef struct {
    char path[260];
    long off;            // byte offset of the image data within the file
    long len;            // image byte length
    bool is_png;
    bool valid;
} cover_src_t;
static cover_src_t g_src;

static FILE *cover_open(long *remain)
{
    if (!g_src.valid) return NULL;
    FILE *f = fopen(g_src.path, "rb");
    if (!f) return NULL;
    if (g_src.off > 0 && fseek(f, g_src.off, SEEK_SET) != 0) { fclose(f); return NULL; }
    *remain = g_src.len;
    return f;
}

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

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    long n = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : 0;
    fclose(f);
    return n < 0 ? 0 : n;
}

int cover_load(const char *path, bool *is_png)
{
    g_src.valid = false;

    long off = 0, len = 0; bool png = false;
    if (id3_locate_cover(path, &off, &len, &png) && len > 8) {
        snprintf(g_src.path, sizeof(g_src.path), "%s", path);
        g_src.off = off; g_src.len = len; g_src.is_png = png; g_src.valid = true;
    } else {
        char side[260]; bool spng = false;
        if (!find_sidecar(path, side, sizeof(side), &spng)) return 0;
        long sz = file_size(side);
        if (sz <= 8) return 0;
        snprintf(g_src.path, sizeof(g_src.path), "%s", side);
        g_src.off = 0; g_src.len = sz; g_src.is_png = spng; g_src.valid = true;
    }
    *is_png = g_src.is_png;
    return (int)(g_src.len > 0x7fffffff ? 0x7fffffff : g_src.len);   // truthy "have cover"
}

// --- JPEG (TJpgDec) ---------------------------------------------------------

typedef struct { FILE *f; long remain; } jpg_src_t;

// tjpgd input: read bytes into buf, or skip them when buf == NULL. Streams from
// the file so the compressed JPEG is never fully resident in RAM.
static size_t jpg_in(JDEC *jd, uint8_t *buf, size_t len)
{
    jpg_src_t *s = (jpg_src_t *)jd->device;
    if (len > (size_t)s->remain) len = (size_t)s->remain;
    if (buf) {
        size_t got = fread(buf, 1, len, s->f);
        s->remain -= (long)got;
        return got;
    }
    if (fseek(s->f, (long)len, SEEK_CUR) != 0) return 0;   // skip
    s->remain -= (long)len;
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

// Forward decls (defined below in the PNG section).
static void blit_rgb_box(const uint8_t *px, int w, int h, int ch,
                         int bx, int by, int bw, int bh);
static void scale_rgb_to_thumb(const uint8_t *px, int w, int h, int ch,
                               uint16_t *out, int sz);

// --- progressive-JPEG fallback (DC-only 1/8-resolution preview) -------------
// TJpgDec rejects progressive JPEGs (JDR_FMT3). progjpeg reconstructs a small
// 1/8-res preview from the first DC scan — plenty for the 112/34px card/thumb.

static int pj_file_get(void *u)
{
    jpg_src_t *s = (jpg_src_t *)u;
    if (s->remain <= 0) return -1;
    int c = fgetc(s->f);
    if (c < 0) return -1;
    s->remain--;
    return c;
}

// Decode the located progressive cover into g_scratch as RGB888. The baseline
// decoder is dead by now, so g_scratch is free as the output buffer.
static bool prog_decode(uint8_t **rgb, int *w, int *h)
{
    long remain; FILE *f = cover_open(&remain);
    if (!f) return false;
    jpg_src_t fs = { f, remain };
    PjSource src = { pj_file_get, &fs };
    int ok = pj_decode_dc(&src, g_scratch, SCRATCH_MAX, w, h);
    fclose(f);
    if (!ok) return false;
    *rgb = g_scratch;
    return true;
}

// Stream-decode the located JPEG cover, scaled to fit (bw,bh), centered at (bx,by).
static bool jpeg_to_box(int n, int bx, int by, int bw, int bh)
{
    (void)n;
    long remain; FILE *f = cover_open(&remain);
    if (!f) return false;

    JDEC jd;
    jpg_src_t src = { f, remain };
    // Borrow g_scratch as the tjpgd work area — PNG decode (its only other user)
    // never runs concurrently with JPEG decode.
    int pr = jd_prepare(&jd, jpg_in, g_scratch, JPEG_WORK_SZ, &src);
    if (pr != JDR_OK) {
        fclose(f);
        if (pr == JDR_FMT3) {                  // progressive → DC preview fallback
            uint8_t *rgb; int w, h;
            if (prog_decode(&rgb, &w, &h)) {
                blit_rgb_box(rgb, w, h, 3, bx, by, bw, bh);
                return true;
            }
        }
        return false;
    }
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
    bool ok = jd_decomp(&jd, jpg_out, sc) == JDR_OK;
    fclose(f);
    return ok;
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

typedef struct { FILE *f; long remain; } file_reader_t;

// lupng input: streams the compressed PNG from the file (decompressed pixels go
// to g_scratch via scratch_alloc).
static size_t file_read(void *out, size_t size, size_t count, void *u)
{
    file_reader_t *r = (file_reader_t *)u;
    size_t want = size * count;
    if (want > (size_t)r->remain) want = (size_t)r->remain;
    size_t got = fread(out, 1, want, r->f);
    r->remain -= (long)got;
    return size ? got / size : 0;
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

// Nearest-scale an 8-bit RGB(A) image into an sz×sz RGB565 thumbnail.
static void scale_rgb_to_thumb(const uint8_t *px, int w, int h, int ch,
                               uint16_t *out, int sz)
{
    for (int y = 0; y < sz; y++) {
        int sy = y * h / sz;
        const uint8_t *row = px + (size_t)sy * w * ch;
        for (int x = 0; x < sz; x++) {
            const uint8_t *p = row + (size_t)(x * w / sz) * ch;
            out[y * sz + x] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        }
    }
}

static bool png_to_box(int n, int bx, int by, int bw, int bh)
{
    (void)n;
    long remain; FILE *f = cover_open(&remain);
    if (!f) return false;
    file_reader_t rd = { f, remain };
    g_scratch_off = 0;

    LuUserContext uc;
    luUserContextInitDefault(&uc);
    uc.readProc = file_read;      uc.readProcUserPtr = &rd;
    uc.allocProc = scratch_alloc; uc.allocProcUserPtr = NULL;
    uc.freeProc = scratch_free;   uc.freeProcUserPtr = NULL;
    uc.warnProc = NULL;

    LuImage *img = luPngReadUC(&uc);
    fclose(f);
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
    (void)n;
    long remain; FILE *f = cover_open(&remain);
    if (!f) return false;

    JDEC jd;
    jpg_src_t src = { f, remain };
    // Same shared work area as jpeg_to_box() — see note there.
    int pr = jd_prepare(&jd, jpg_in, g_scratch, JPEG_WORK_SZ, &src);
    if (pr != JDR_OK) {
        fclose(f);
        if (pr == JDR_FMT3) {                  // progressive → DC preview fallback
            uint8_t *rgb; int w, h;
            if (prog_decode(&rgb, &w, &h)) { scale_rgb_to_thumb(rgb, w, h, 3, out, sz); return true; }
        }
        return false;
    }
    uint8_t sc = 0;
    while (sc < 3 && (((int)jd.width >> sc) > sz * 4 || ((int)jd.height >> sc) > sz * 4)) sc++;
    g_thumb_sw = (int)jd.width >> sc;
    g_thumb_sh = (int)jd.height >> sc;
    g_thumb_dst = out;
    g_thumb_sz = sz;
    memset(out, 0, (size_t)sz * sz * sizeof(uint16_t));
    bool ok = jd_decomp(&jd, jpg_thumb_out, sc) == JDR_OK;
    fclose(f);
    return ok;
}

static bool png_to_thumb(int n, uint16_t *out, int sz)
{
    (void)n;
    long remain; FILE *f = cover_open(&remain);
    if (!f) return false;
    file_reader_t rd = { f, remain };
    g_scratch_off = 0;

    LuUserContext uc;
    luUserContextInitDefault(&uc);
    uc.readProc = file_read;      uc.readProcUserPtr = &rd;
    uc.allocProc = scratch_alloc; uc.allocProcUserPtr = NULL;
    uc.freeProc = scratch_free;   uc.freeProcUserPtr = NULL;
    uc.warnProc = NULL;

    LuImage *img = luPngReadUC(&uc);
    fclose(f);
    if (!img) return false;
    if (img->depth != 8 || img->channels < 3) return false;

    scale_rgb_to_thumb(img->data, img->width, img->height, img->channels, out, sz);
    return true;
}

bool cover_thumb(const char *path, uint16_t *out, int sz)
{
    // Decoded on demand for visible rows; the caller keeps a small RAM ring
    // (g_meta) so on-screen thumbnails are not re-decoded. No on-disk cache.
    bool is_png = false;
    int n = cover_load(path, &is_png);
    if (n <= 0) return false;
    return is_png ? png_to_thumb(n, out, sz) : jpeg_to_thumb(n, out, sz);
}
