/* Host unit tests for Core/Src/porting/video/avi.c — the AVI/RIFF demuxer that
 * feeds the Video player. avi.c is pure logic over FILE* (no hardware), so the
 * whole chunk-walk / read / frame-count / seek machinery is host-testable.
 *
 * This is the regression guard behind the read-path the prefetch instrumentation
 * (rd=/pf= HUD split) sits on: if chunk walking, payload reads, frame counting or
 * seeking drift, the player's overlap accounting silently lies. Build a minimal
 * in-memory AVI, write it to a temp file, and assert the demuxer's behaviour.
 *
 * Compile + run (also recorded in tests/test_avi.build):
 *   gcc -O2 -Wall -Wextra -std=gnu11 -ICore/Inc/porting/video \
 *       tests/test_avi.c Core/Src/porting/video/avi.c -o /tmp/mtest/test_avi
 *   /tmp/mtest/test_avi
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "avi.h"

/* ---- tiny test harness (matches tests/test_storage.c conventions) -------- */
static int g_failures = 0;
#define CHECK(cond, msg)                              \
    do {                                              \
        if (!(cond)) {                                \
            printf("  FAIL: %s\n", (msg));            \
            g_failures++;                             \
        }                                             \
    } while (0)
#define OK(name) printf("OK %s\n", (name))

/* ---- little AVI builder --------------------------------------------------- */
/* A growable byte buffer we assemble the RIFF into, then flush to a temp file. */
typedef struct { uint8_t *p; size_t n, cap; } buf_t;

static void b_need(buf_t *b, size_t extra)
{
    if (b->n + extra <= b->cap) return;
    while (b->cap < b->n + extra) b->cap = b->cap ? b->cap * 2 : 256;
    b->p = realloc(b->p, b->cap);
    if (!b->p) { perror("realloc"); exit(2); }
}
static void b_bytes(buf_t *b, const void *src, size_t n)
{
    b_need(b, n);
    memcpy(b->p + b->n, src, n);
    b->n += n;
}
static void b_fourcc(buf_t *b, const char *cc) { b_bytes(b, cc, 4); }
static void b_u32(buf_t *b, uint32_t v)
{
    uint8_t e[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    b_bytes(b, e, 4);
}
/* patch a previously-reserved u32 (for LIST/RIFF sizes) */
static void b_patch_u32(buf_t *b, size_t at, uint32_t v)
{
    b->p[at]   = (uint8_t)v;
    b->p[at + 1] = (uint8_t)(v >> 8);
    b->p[at + 2] = (uint8_t)(v >> 16);
    b->p[at + 3] = (uint8_t)(v >> 24);
}

/* Emit a data chunk ("NNxx" + size + payload + pad-to-even). */
static void b_chunk(buf_t *b, const char *cc, const uint8_t *payload, uint32_t len)
{
    b_fourcc(b, cc);
    b_u32(b, len);
    b_bytes(b, payload, len);
    if (len & 1) { uint8_t z = 0; b_bytes(b, &z, 1); }   /* RIFF word alignment */
}

/* Build the test clip; returns the temp-file path (static storage). */
#define UPF   41708u        /* ~23.976 fps -> avi_frame_ms() == 42 */
#define NVID  4             /* video frames in the movi list */

static const char *build_avi(void)
{
    buf_t b = {0};

    /* RIFF/AVI header */
    b_fourcc(&b, "RIFF");
    size_t riff_size_at = b.n; b_u32(&b, 0);      /* patched last */
    b_fourcc(&b, "AVI ");

    /* LIST hdrl { avih(56) } */
    b_fourcc(&b, "LIST");
    size_t hdrl_size_at = b.n; b_u32(&b, 0);
    size_t hdrl_body = b.n;
    b_fourcc(&b, "hdrl");
    b_fourcc(&b, "avih");
    b_u32(&b, 56);
    size_t avih = b.n;
    for (int i = 0; i < 56; i++) { uint8_t z = 0; b_bytes(&b, &z, 1); }
    b_patch_u32(&b, avih + 0,  UPF);              /* dwMicroSecPerFrame */
    b_patch_u32(&b, avih + 16, NVID);             /* dwTotalFrames      */
    b_patch_u32(&b, avih + 32, 320);              /* dwWidth            */
    b_patch_u32(&b, avih + 36, 240);              /* dwHeight           */
    b_patch_u32(&b, hdrl_size_at, (uint32_t)(b.n - hdrl_body));

    /* LIST movi { 00dc, 01wb, 00dc, 00dc(odd), 00dc } */
    b_fourcc(&b, "LIST");
    size_t movi_size_at = b.n; b_u32(&b, 0);
    size_t movi_body = b.n;
    b_fourcc(&b, "movi");

    /* frame payloads: FF D8 <frame-index> then filler; sizes vary; frame 2 odd. */
    for (int f = 0; f < NVID; f++) {
        uint32_t len = 8 + (uint32_t)f * 3;       /* 8,11,14,17 -> f==1,3 are odd */
        uint8_t pay[64];
        pay[0] = 0xFF; pay[1] = 0xD8; pay[2] = (uint8_t)f;
        for (uint32_t i = 3; i < len; i++) pay[i] = (uint8_t)(0xA0 + f);
        b_chunk(&b, "00dc", pay, len);
        if (f == 0) {                             /* one audio chunk after frame 0 */
            uint8_t au[6] = { 0xFF, 0xFB, 0x11, 0x22, 0x33, 0x44 };
            b_chunk(&b, "01wb", au, sizeof au);
        }
    }
    b_patch_u32(&b, movi_size_at, (uint32_t)(b.n - movi_body));
    b_patch_u32(&b, riff_size_at, (uint32_t)(b.n - 8));

    static char path[] = "/tmp/mtest/test_avi_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    if (write(fd, b.p, b.n) != (ssize_t)b.n) { perror("write"); exit(2); }
    close(fd);
    free(b.p);
    return path;
}

/* ------------------------------------------------------------------------- */

static void test_header_parse(const char *path)
{
    avi_t a;
    CHECK(avi_open(&a, path, NULL, 0), "avi_open on a valid clip");
    CHECK(a.width == 320 && a.height == 240, "header width/height");
    CHECK(a.usec_per_frame == (int)UPF, "header usec_per_frame");
    CHECK(a.total_frames == NVID, "header total_frames");
    CHECK(avi_frame_ms(&a) == 42, "avi_frame_ms rounds 41708us -> 42ms");
    avi_close(&a);
    OK("header parse");
}

/* Walk the whole movi list, verifying kinds, sizes, payload bytes and the
 * video frame counter that seeking relies on. */
static void test_walk_and_read(const char *path)
{
    avi_t a;
    CHECK(avi_open(&a, path, NULL, 0), "reopen for walk");

    int video = 0, audio = 0;
    long sz;
    avi_kind_t k;
    while ((k = avi_next(&a, &sz)) != AVI_END) {
        uint8_t p[64] = {0};
        size_t got = avi_read(&a, p, (size_t)sz);
        CHECK(got == (size_t)sz, "avi_read returns the whole chunk payload");
        if (k == AVI_VIDEO) {
            CHECK(p[0] == 0xFF && p[1] == 0xD8, "video payload starts FFD8");
            CHECK(p[2] == (uint8_t)video, "video payload carries its frame index");
            CHECK(sz == 8 + (long)video * 3, "video chunk size as authored");
            CHECK(a.cur_frame == video + 1, "cur_frame advances one per video frame");
            video++;
        } else {
            CHECK(p[0] == 0xFF && p[1] == 0xFB, "audio payload starts FFFB");
            audio++;
        }
    }
    CHECK(video == NVID, "saw every video frame (odd-size padding walked)");
    CHECK(audio == 1, "saw the one audio chunk");
    avi_close(&a);
    OK("walk + read (incl. odd-size padding & cur_frame)");
}

/* Seeking: forward to a mid frame, then backward to 0. Works even with NO idx1
 * present (the demuxer walks from the nearest playback checkpoint / movi start). */
static void test_seek(const char *path)
{
    avi_t a;
    CHECK(avi_open(&a, path, NULL, 0), "reopen for seek");

    long sz;
    avi_seek_frame(&a, 2);                        /* next frame should be index 2 */
    CHECK(avi_next(&a, &sz) == AVI_VIDEO, "post-seek chunk is a video frame");
    uint8_t p[64] = {0};
    avi_read(&a, p, (size_t)sz);
    CHECK(p[2] == 2, "forward seek(2) lands on frame index 2");

    avi_seek_frame(&a, 0);                        /* backward to the start */
    CHECK(avi_next(&a, &sz) == AVI_VIDEO, "post-rewind chunk is a video frame");
    memset(p, 0, sizeof p);
    avi_read(&a, p, (size_t)sz);
    CHECK(p[2] == 0, "backward seek(0) lands on frame index 0");

    /* clamp: seeking past the end must not run away */
    avi_seek_frame(&a, 999);
    CHECK(a.cur_frame <= NVID, "seek past end clamps to the frame count");
    avi_close(&a);
    OK("seek forward / backward / clamp (no idx1)");
}

static void test_bad_open(void)
{
    avi_t a;
    CHECK(!avi_open(&a, "/tmp/mtest/does_not_exist_avi", NULL, 0),
          "avi_open on a missing file fails cleanly");
    OK("bad open");
}

int main(void)
{
    printf("== test_avi ==\n");
    const char *path = build_avi();
    test_header_parse(path);
    test_walk_and_read(path);
    test_seek(path);
    test_bad_open();
    unlink(path);

    if (g_failures) { printf("FAILED (%d)\n", g_failures); return 1; }
    printf("all avi tests passed\n");
    return 0;
}
