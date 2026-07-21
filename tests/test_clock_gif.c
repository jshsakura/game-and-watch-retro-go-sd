/* Host test for the Clock GIF pipeline: compiles the REAL gifdec + rg_clock_gif
 * with a simulated emu-RAM pool AND a simulated clock_overlay_arena() (the
 * decode arena past .overlay_clock's own footprint — see rg_clock_gif.c),
 * then decodes a real animated GIF end to end, checking frames actually
 * change and the blit produces plausible pixels. Catches decoder/allocator
 * regressions the device can only report as a black background. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define GW_LCD_WIDTH  320
#define GW_LCD_HEIGHT 240

/* ---- simulated emu-RAM bump pool (gw_malloc) ---- */
static uint8_t  pool[700 * 1024];
static size_t   pool_off = 0;
void  *ram_malloc(size_t n) { n = (n + 3) & ~3u;
    if (pool_off + n > sizeof pool) return NULL;
    void *p = pool + pool_off; pool_off += n; return p; }
void  *ram_calloc(size_t c, size_t s) { void *p = ram_malloc(c * s); if (p) memset(p, 0, c * s); return p; }
size_t ram_get_free_size(void) { return sizeof pool - pool_off; }
size_t ram_mark(void) { return pool_off + 1; }             /* +1: never 0 */
void   ram_release(size_t m) { pool_off = m - 1; }

/* ---- simulated clock_overlay_arena() (rg_clock_ring.c) — the decode arena
 * past .overlay_clock's own footprint, sized generously (527 KB). The size
 * override lets one test fake a buffer too small for the GIF. ---- */
static uint8_t shared_buf[527 * 1024];
static size_t  shared_bytes_override = 0;
uint8_t *clock_overlay_arena(size_t *out_bytes)
{ if (out_bytes) *out_bytes = shared_bytes_override ? shared_bytes_override : sizeof shared_buf;
  return shared_buf; }

/* boot call site is a no-op now (real definition moved to rg_clock_ring.c) */
void clock_gif_reserve(void) {}

/* redirect the absolute SD path to the test file */
static const char *gif_path(const char *p)
{ return strcmp(p, "/clock/gif/bg.gif") == 0 ? "/tmp/mtest/bg.gif" : p; }
#define open(p, f) open(gif_path(p), f)
#include "../Core/Src/porting/lib/gifdec/gifdec.c"
#include "../Core/Src/retro-go/rg_clock_gif.c"   /* probe open() redirected too */
#undef open

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS  %s\n", name); \
    else { printf("FAIL  %s\n", name); fails++; } } while (0)

static uint32_t fb_sum(const uint16_t *fb)
{ uint32_t s = 0; for (int i = 0; i < GW_LCD_WIDTH*GW_LCD_HEIGHT; i++) s += fb[i]; return s; }

int main(void)
{
    static uint16_t fb[GW_LCD_WIDTH * GW_LCD_HEIGHT];
    size_t before = pool_off;

    clock_gif_reserve();   /* boot call site is a no-op now */
    CHECK(pool_off == before, "reserve is free (no emu-RAM claimed)");

    CHECK(clock_gif_load(), "gif loads from the shared_files arena");
    CHECK(clock_gif_ready(), "ready after load");
    CHECK(pool_off == before, "emu-RAM pool untouched by load");

    memset(fb, 0, sizeof fb);
    clock_gif_blit(fb, 0);
    uint32_t s1 = fb_sum(fb);
    CHECK(s1 != 0, "first frame blits non-black pixels");

    /* advance past several frame delays; content must change */
    uint32_t sums[6]; int distinct = 1; sums[0] = s1;
    uint32_t t = 0;
    for (int i = 1; i < 6; i++) {
        t += 150;                       /* > per-frame delay (100 ms) */
        clock_gif_blit(fb, t);
        sums[i] = fb_sum(fb);
        int is_new = 1;
        for (int j = 0; j < i; j++) if (sums[j] == sums[i]) { is_new = 0; break; }
        distinct += is_new;
    }
    printf("  distinct frame checksums over 6 blits: %d\n", distinct);
    CHECK(distinct >= 4, "animation advances (frames differ)");

    /* loop past the end: 8 frames x 100 ms, go to 3 seconds */
    for (; t < 3000; t += 150) clock_gif_blit(fb, t);
    CHECK(fb_sum(fb) != 0, "still rendering after looping");

    clock_gif_free();
    CHECK(pool_off == before, "pool still untouched after free");
    CHECK(!clock_gif_ready(), "not ready after free");

    /* reload after free must work (arena reset round trip) */
    CHECK(clock_gif_load(), "reload after free");
    clock_gif_blit(fb, 5000);
    CHECK(fb_sum(fb) != 0, "reload renders");
    clock_gif_free();

    /* --- the field failure: launcher covers filled the emu-RAM pool down to
     *     41K free ("no RAM: need 273K free 41K"). The shared_files arena is
     *     independent of the pool, so the GIF must STILL load. --- */
    while (ram_get_free_size() > 41 * 1024)     /* covers eat the pool */
        if (!ram_malloc(4096)) break;
    printf("  pool free now: %zu KB\n", ram_get_free_size() / 1024);
    CHECK(clock_gif_load(), "gif loads with 41K pool free (shared arena)");
    memset(fb, 0, sizeof fb);
    clock_gif_blit(fb, 0);
    CHECK(fb_sum(fb) != 0, "frame renders under pool pressure");
    clock_gif_free();

    /* a too-small shared buffer fails cleanly (not fatally) */
    shared_bytes_override = 100 * 1024;         /* < 273K needed for 320x240 */
    CHECK(!clock_gif_load(), "load fails cleanly when the arena is too small");
    CHECK(clock_gif_status() == CLOCK_GIF_NO_RAM, "status reports NO_RAM");
    CHECK(!clock_gif_ready(), "not ready after failed load");
    shared_bytes_override = 0;
    CHECK(clock_gif_load(), "recovers once the arena is big enough");
    clock_gif_free();

    printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
