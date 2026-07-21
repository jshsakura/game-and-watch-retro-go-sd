/* Host unit tests for Core/Src/porting/video/video_decode.c: the JPEG-header
 * size walk (jpeg_dims(), reached only through video_decode_slot()'s public
 * API — it is static) and the g_scratch slot layout (video_slot()).
 *
 * Compiles the REAL video_decode.c. The only hardware seam is the HW JPEG
 * peripheral itself (hw_jpeg_decoder.h's JPEG_DecodeTo*): stubbed to succeed
 * trivially, since none of the cases here (malformed/truncated/oversized
 * headers) ever reach it — video_decode_slot() rejects those before calling
 * the peripheral. g_scratch is a real 352KB buffer (SCRATCH_MAX, matching
 * music_cover.c) so the slot-layout pointer arithmetic operates on real,
 * addressable memory.
 *
 * Compile + run (also in tests/run.sh / tests/coverage.sh):
 *   gcc -O2 -Wall -Wextra -std=gnu11 -Itests/video_stubs -ICore/Inc/porting/video \
 *       -ICore/Src/porting/lib -ICore/Inc/porting/music \
 *       tests/test_video_decode.c Core/Src/porting/video/video_decode.c \
 *       -o /tmp/mtest/test_video_decode
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "video_decode.h"

/* Diagnostics globals defined in video_decode.c (see its own comment for the
 * st= code table); not part of video_decode.h's public API but this is
 * exactly what the tests below assert on. */
extern int g_vdec_st, g_vdec_w, g_vdec_h;

/* ---- hardware seams --------------------------------------------------- */
void wdog_refresh(void) {}
uint32_t HAL_GetTick(void) { return 0; }
void SCB_CleanDCache_by_Addr(uint32_t *addr, int32_t dsize) { (void)addr; (void)dsize; }

static int g_jpeg_decode_calls = 0;
static uint32_t g_jpeg_decode_rc = 0;   /* test-controlled return */
uint32_t JPEG_DecodeToFrameInit(uint32_t buf, uint32_t sz) { (void)buf; (void)sz; return 0; }
uint32_t JPEG_DecodeDeInit(void) { return 0; }
uint32_t JPEG_DecodeToFrame(uint32_t src, uint32_t sz, uint32_t dst, uint16_t x, uint16_t y, uint8_t la)
{
    (void)src; (void)sz; (void)dst; (void)x; (void)y; (void)la;
    g_jpeg_decode_calls++;
    return g_jpeg_decode_rc;
}

#define SCRATCH_MAX (352 * 1024)   /* matches music_cover.c's g_scratch sizing */
uint8_t g_scratch[SCRATCH_MAX];

/* ---- tiny test harness (matches tests/test_avi.c conventions) ------------ */
static int g_failures = 0;
#define CHECK(cond, msg)                              \
    do {                                              \
        if (!(cond)) {                                \
            printf("  FAIL: %s\n", (msg));            \
            g_failures++;                             \
        }                                             \
    } while (0)
#define OK(name) printf("OK %s\n", (name))

#define FB_W 320
#define FB_H 240
static uint16_t fb[FB_W * FB_H];

/* ---- JPEG buffer builder --------------------------------------------------
 * Just enough of the marker stream for jpeg_dims()'s walk: SOI, then a
 * caller-chosen sequence of markers, ending (optionally) with an SOFn segment
 * carrying width/height at the offsets jpeg_dims() reads directly. */
typedef struct { uint8_t b[512]; int n; } jb_t;
static void jb_init(jb_t *j) { j->n = 0; j->b[j->n++] = 0xFF; j->b[j->n++] = 0xD8; }   /* SOI */
static void jb_bytes(jb_t *j, const uint8_t *p, int n) { memcpy(j->b + j->n, p, n); j->n += n; }
/* A length-prefixed segment (APPn/DQT/...) jpeg_dims() must skip via i += 2+len. */
static void jb_segment(jb_t *j, uint8_t marker, int payload_len)
{
    uint8_t hdr[4] = { 0xFF, marker, (uint8_t)((payload_len + 2) >> 8), (uint8_t)((payload_len + 2) & 0xFF) };
    jb_bytes(j, hdr, 4);
    for (int i = 0; i < payload_len; i++) { uint8_t z = 0xAA; jb_bytes(j, &z, 1); }
}
/* SOFn (0xC0/0xC1/0xC2) carrying width/height at the exact offsets read. */
static void jb_sof(jb_t *j, uint8_t sof_marker, int w, int h)
{
    uint8_t seg[9] = {
        0xFF, sof_marker, 0, 11,               /* length: precision+h+w+1 component (not read) */
        8,                                       /* precision (byte i+4, unread but present) */
        (uint8_t)(h >> 8), (uint8_t)(h & 0xFF), /* i+5,i+6 */
        (uint8_t)(w >> 8), (uint8_t)(w & 0xFF), /* i+7,i+8 */
    };
    jb_bytes(j, seg, sizeof seg);
    uint8_t pad[8] = {0};                        /* the "i+9 < n" margin jpeg_dims() needs */
    jb_bytes(j, pad, sizeof pad);
}

/* --------------------------------------------------------------------------
 * video_decode_slot()'s size/pointer argument checks (st=1) — before any
 * header parsing at all.
 * -------------------------------------------------------------------------- */
static void test_bad_args(void)
{
    jb_t j; jb_init(&j); jb_sof(&j, 0xC0, 100, 80);

    CHECK(!video_decode_slot(NULL, j.n, fb, FB_W, FB_H), "NULL src rejected");
    CHECK(g_vdec_st == 1, "NULL src -> st=1");

    CHECK(!video_decode_slot(j.b, 1, fb, FB_W, FB_H), "size < 2 rejected");
    CHECK(g_vdec_st == 1, "size=1 -> st=1");

    CHECK(!video_decode_slot(j.b, 0, fb, FB_W, FB_H), "size == 0 rejected");
    CHECK(g_vdec_st == 1, "size=0 -> st=1");

    CHECK(!video_decode_slot(j.b, VIDEO_FRAME_MAX + 1, fb, FB_W, FB_H), "size > FRAME_MAX rejected");
    CHECK(g_vdec_st == 1, "size=FRAME_MAX+1 -> st=1");

    CHECK(!video_decode_slot(j.b, j.n, NULL, FB_W, FB_H), "NULL fb rejected");
    CHECK(g_vdec_st == 1, "NULL fb -> st=1");
    OK("bad-args / oversized / undersized rejected as st=1, before any parsing");
}

/* --------------------------------------------------------------------------
 * jpeg_dims() failure paths -> st=3 (via video_decode_slot()'s public face).
 * -------------------------------------------------------------------------- */
static void test_malformed_markers(void)
{
    uint8_t garbage[32];
    memset(garbage, 0x00, sizeof garbage);   /* no 0xFF anywhere: no marker ever found */
    CHECK(!video_decode_slot(garbage, sizeof garbage, fb, FB_W, FB_H), "buffer with no marker at all rejected");
    CHECK(g_vdec_st == 3, "no marker found -> st=3 (jpeg_dims fails)");
    OK("malformed marker stream (no SOF found) -> st=3");
}

static void test_truncated_header(void)
{
    jb_t j; jb_init(&j);
    /* SOI, then a truncated SOF0: marker+length present but not enough bytes
     * for jpeg_dims()'s i+9 < n read window. */
    uint8_t partial[6] = { 0xFF, 0xC0, 0, 11, 8, 0 };
    jb_bytes(&j, partial, sizeof partial);
    CHECK(!video_decode_slot(j.b, j.n, fb, FB_W, FB_H), "truncated SOF0 rejected");
    CHECK(g_vdec_st == 3, "truncated header -> st=3");
    OK("truncated header (SOF cut short) -> st=3");
}

static void test_no_sof_present(void)
{
    jb_t j; jb_init(&j);
    jb_segment(&j, 0xE0, 14);    /* APP0 (JFIF) */
    jb_segment(&j, 0xDB, 65);    /* DQT */
    jb_segment(&j, 0xC4, 20);    /* DHT */
    /* ...ends without ever hitting an SOF0/1/2 marker. */
    CHECK(!video_decode_slot(j.b, j.n, fb, FB_W, FB_H), "stream with markers but no SOF rejected");
    CHECK(g_vdec_st == 3, "no SOF in a well-formed-looking stream -> st=3");
    OK("no SOF marker present (segments walked, none is SOFn) -> st=3");
}

static void test_zero_dims_rejected(void)
{
    jb_t j; jb_init(&j); jb_sof(&j, 0xC0, 0, 100);   /* width encoded as 0 */
    CHECK(!video_decode_slot(j.b, j.n, fb, FB_W, FB_H), "zero width rejected");
    CHECK(g_vdec_st == 3, "w=0 -> jpeg_dims returns false (w>0 required) -> st=3");
    OK("zero-encoded dimension -> st=3");
}

/* --------------------------------------------------------------------------
 * Dimensions parsed fine but larger than the screen -> st=4 (HW codec can't
 * downscale). Sizes at/under the frame budget succeed through to the (stubbed)
 * HW decode -> st=0.
 * -------------------------------------------------------------------------- */
static void test_larger_than_screen(void)
{
    jb_t j; jb_init(&j); jb_sof(&j, 0xC0, FB_W + 1, 100);   /* width exceeds the framebuffer */
    CHECK(!video_decode_slot(j.b, j.n, fb, FB_W, FB_H), "wider-than-screen frame rejected");
    CHECK(g_vdec_st == 4, "w>fb_w -> st=4");
    CHECK(g_vdec_w == FB_W + 1, "parsed width is still reported for diagnostics");

    jb_t j2; jb_init(&j2); jb_sof(&j2, 0xC0, 100, FB_H + 1);
    CHECK(!video_decode_slot(j2.b, j2.n, fb, FB_W, FB_H), "taller-than-screen frame rejected");
    CHECK(g_vdec_st == 4, "h>fb_h -> st=4");
    OK("dimensions larger than the screen -> st=4");
}

static void test_successful_decode(void)
{
    jb_t j; jb_init(&j); jb_sof(&j, 0xC0, 100, 80);
    g_jpeg_decode_rc = 0;   /* stubbed HW decode reports success */
    int calls_before = g_jpeg_decode_calls;
    CHECK(video_decode_slot(j.b, j.n, fb, FB_W, FB_H), "in-budget frame decodes");
    CHECK(g_vdec_st == 0, "successful decode -> st=0");
    CHECK(g_vdec_w == 100 && g_vdec_h == 80, "parsed dims match what was encoded");
    CHECK(g_jpeg_decode_calls == calls_before + 1, "reaches the (stubbed) HW decode exactly once");
    OK("in-budget, well-formed frame reaches HW decode and reports st=0");

    /* HW decode itself reporting failure -> st=5 (a genuinely different
     * failure mode from every parsing rejection above — CLAUDE.md's table of
     * st codes exists precisely so these don't get conflated on screen). */
    g_jpeg_decode_rc = 1;
    CHECK(!video_decode_slot(j.b, j.n, fb, FB_W, FB_H), "HW decode failure propagates");
    CHECK(g_vdec_st == 5, "HW rc!=0 -> st=5");
    OK("HW decode rejection (rc!=0) -> st=5, distinct from every parse failure");
}

/* SOF1 (0xC1, extended sequential) and SOF2 (0xC2, progressive) must parse
 * identically to SOF0 — jpeg_dims() treats all three the same. */
static void test_sof_variants(void)
{
    jb_t j1; jb_init(&j1); jb_sof(&j1, 0xC1, 64, 48);
    g_jpeg_decode_rc = 0;
    CHECK(video_decode_slot(j1.b, j1.n, fb, FB_W, FB_H), "SOF1 parses");
    CHECK(g_vdec_w == 64 && g_vdec_h == 48, "SOF1 dims parsed correctly");

    jb_t j2; jb_init(&j2); jb_sof(&j2, 0xC2, 64, 48);
    CHECK(video_decode_slot(j2.b, j2.n, fb, FB_W, FB_H), "SOF2 (progressive) parses");
    CHECK(g_vdec_w == 64 && g_vdec_h == 48, "SOF2 dims parsed correctly");
    OK("SOF0 / SOF1 / SOF2 all parsed the same way");
}

/* A standalone RST/TEM marker before the SOF must be skipped as a 2-byte
 * marker (no length field), not misread as a length-prefixed segment. */
static void test_standalone_marker_skip(void)
{
    jb_t j; jb_init(&j);
    uint8_t rst[2] = { 0xFF, 0xD0 };   /* RST0: standalone, no length */
    jb_bytes(&j, rst, sizeof rst);
    jb_sof(&j, 0xC0, 50, 40);
    g_jpeg_decode_rc = 0;
    CHECK(video_decode_slot(j.b, j.n, fb, FB_W, FB_H), "SOF after a standalone marker parses");
    CHECK(g_vdec_w == 50 && g_vdec_h == 40, "dims correct despite the leading standalone marker");
    OK("standalone (no-length) markers are skipped as 2 bytes, not a length-prefixed segment");
}

/* --------------------------------------------------------------------------
 * video_slot() layout: pinned to the documented g_scratch partition (see this
 * file's own header comment) so a future edit that overlaps two regions is a
 * test failure, not a corrupted decode discovered on hardware. JWORK_SZ
 * (160*1024) is file-local to video_decode.c, not exported — mirrored here
 * from its own comment ">= 320x240 4:2:0 (115KB) with margin", the same
 * cross-file-constant pattern tests/test_video_audio.c uses for
 * PF_AUDIO_HEADROOM. If either changes, this test and that comment must move
 * together.
 * -------------------------------------------------------------------------- */
static void test_slot_layout_no_overlap(void)
{
    const long JWORK_SZ = 160 * 1024;
    uint8_t *s0 = video_slot(0), *s1 = video_slot(1), *s2 = video_slot(2);

    CHECK(s0 == g_scratch, "slot 0 sits at the base of g_scratch");
    CHECK(s1 == g_scratch + 224 * 1024, "slot 1 sits right after the JPEG work area");
    CHECK(s2 == g_scratch + 288 * 1024, "slot 2 sits right after slot 1");
    CHECK(s2 + VIDEO_FRAME_MAX <= g_scratch + SCRATCH_MAX, "slot 2's frame budget fits inside g_scratch");

    /* Non-overlap, computed from first principles (not the literal offsets
     * above) so a change to VIDEO_FRAME_MAX or the work-area size is checked
     * too, not just the three hardcoded addresses. */
    long work_start = VIDEO_FRAME_MAX;             /* video_decode_init() point */
    long work_end   = work_start + JWORK_SZ;
    long s0_start = s0 - g_scratch, s0_end = s0_start + VIDEO_FRAME_MAX;
    long s1_start = s1 - g_scratch, s1_end = s1_start + VIDEO_FRAME_MAX;
    long s2_start = s2 - g_scratch, s2_end = s2_start + VIDEO_FRAME_MAX;

    CHECK(s0_end <= work_start, "slot 0's frame budget does not reach into the JPEG work area");
    CHECK(s1_start >= work_end, "slot 1 does not start inside the JPEG work area");
    CHECK(s1_end <= s2_start, "slot 1's frame budget does not reach into slot 2");
    CHECK(s2_end <= SCRATCH_MAX, "slot 2's frame budget does not run past the end of g_scratch");
    OK("video_slot() layout: 3 frame slots + the JPEG work area, no two regions overlap");
}

int main(void)
{
    printf("== test_video_decode ==\n");
    video_decode_init();

    test_bad_args();
    test_malformed_markers();
    test_truncated_header();
    test_no_sof_present();
    test_zero_dims_rejected();
    test_larger_than_screen();
    test_successful_decode();
    test_sof_variants();
    test_standalone_marker_skip();
    test_slot_layout_no_overlap();

    video_decode_deinit();

    if (g_failures) { printf("FAILED (%d)\n", g_failures); return 1; }
    printf("all video_decode tests passed\n");
    return 0;
}
