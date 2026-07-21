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
/* strdup()/mkstemp()/fileno() are POSIX, not standard C; tests/run.sh compiles
 * this with -std=c11 (strict), which hides their declarations otherwise. An
 * implicit declaration wouldn't just warn here -- strdup() returns a pointer,
 * and an implicit declaration assumes `int`, silently truncating it on a
 * 64-bit host. */
#define _POSIX_C_SOURCE 200809L
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

    /* heap, not `static char[]`: build_avi() is now called more than once per
     * process (test_reopen_gives_up_when_file_is_gone() and
     * test_reopen_reapplies_rabuf() each want their own fresh clip), and
     * mkstemp() overwrites its template in place -- a shared static buffer
     * would only work for the first call. */
    char *path = strdup("/tmp/mtest/test_avi_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    if (write(fd, b.p, b.n) != (ssize_t)b.n) { perror("write"); exit(2); }
    close(fd);
    free(b.p);
    return path;
}

/* Flush a buf_t's bytes to a fresh temp file (used by the malformed-header /
 * raw-bytes builders below, which don't go through the RIFF chunk helpers for
 * every byte). */
static const char *flush_to_tmp(buf_t *b)
{
    /* heap, not `static char[]`: this is called several times per test
     * function (unlike build_avi()'s single-shot original), and mkstemp()
     * overwrites its template in place -- a shared static buffer would only
     * work once per process. */
    char *path = strdup("/tmp/mtest/test_avi_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    if (write(fd, b->p, b->n) != (ssize_t)b->n) { perror("write"); exit(2); }
    close(fd);
    free(b->p);
    return path;
}

/* A clip with more frames (so ckpt_step > 1) and an 'idx1' index after movi —
 * exercises avi_build_index()'s lazy idx1 scan, which build_avi() above never
 * reaches (it has no idx1 at all, so seeking falls back to the pure
 * walk-from-checkpoint path already covered by test_seek). idx1 offsets here
 * are movi-relative (relative to the 'movi' fourcc itself, i.e. movi_start-4
 * — the first offset base avi_build_index() tries), matching how real AVI
 * MJPEG encoders on this project emit it. */
#define NVID_IDX 300

static const char *build_avi_with_idx1(void)
{
    buf_t b = {0};
    b_fourcc(&b, "RIFF");
    size_t riff_size_at = b.n; b_u32(&b, 0);
    b_fourcc(&b, "AVI ");

    b_fourcc(&b, "LIST");
    size_t hdrl_size_at = b.n; b_u32(&b, 0);
    size_t hdrl_body = b.n;
    b_fourcc(&b, "hdrl");
    b_fourcc(&b, "avih");
    b_u32(&b, 56);
    size_t avih = b.n;
    for (int i = 0; i < 56; i++) { uint8_t z = 0; b_bytes(&b, &z, 1); }
    b_patch_u32(&b, avih + 0,  UPF);
    b_patch_u32(&b, avih + 16, NVID_IDX);
    b_patch_u32(&b, avih + 32, 320);
    b_patch_u32(&b, avih + 36, 240);
    b_patch_u32(&b, hdrl_size_at, (uint32_t)(b.n - hdrl_body));

    b_fourcc(&b, "LIST");
    size_t movi_size_at = b.n; b_u32(&b, 0);
    size_t movi_body = b.n;
    long movi_fourcc_pos = (long)movi_body;   /* where "movi" itself starts (avi_open's movi_start-4) */
    b_fourcc(&b, "movi");

    static long chunk_pos[NVID_IDX];
    for (int f = 0; f < NVID_IDX; f++) {
        chunk_pos[f] = (long)b.n;                    /* position of this "00dc" fourcc */
        uint8_t pay[8] = { 0xFF, 0xD8, (uint8_t)f, (uint8_t)(f >> 8), 0, 0, 0, 0 };
        b_chunk(&b, "00dc", pay, sizeof pay);
    }
    b_patch_u32(&b, movi_size_at, (uint32_t)(b.n - movi_body));

    /* idx1: one 16-byte entry per video frame, movi-relative offsets. */
    b_fourcc(&b, "idx1");
    size_t idx_size_at = b.n; b_u32(&b, 0);
    size_t idx_body = b.n;
    for (int f = 0; f < NVID_IDX; f++) {
        b_fourcc(&b, "00dc");
        b_u32(&b, 0x10);                              /* AVIIF_KEYFRAME-ish flags, unused by avi.c */
        b_u32(&b, (uint32_t)(chunk_pos[f] - movi_fourcc_pos));
        b_u32(&b, 8);
    }
    b_patch_u32(&b, idx_size_at, (uint32_t)(b.n - idx_body));
    b_patch_u32(&b, riff_size_at, (uint32_t)(b.n - 8));

    return flush_to_tmp(&b);
}

/* Same clip, but a corrupted first idx1 entry's id/offset so no candidate
 * base (movi-relative or file-absolute) confirms — avi_build_index() must
 * leave ckpt[] as recorded during playback instead of trusting garbage. */
static const char *build_avi_with_bad_idx1(void)
{
    buf_t b = {0};
    b_fourcc(&b, "RIFF");
    size_t riff_size_at = b.n; b_u32(&b, 0);
    b_fourcc(&b, "AVI ");
    b_fourcc(&b, "LIST");
    size_t hdrl_size_at = b.n; b_u32(&b, 0);
    size_t hdrl_body = b.n;
    b_fourcc(&b, "hdrl");
    b_fourcc(&b, "avih");
    b_u32(&b, 56);
    size_t avih = b.n;
    for (int i = 0; i < 56; i++) { uint8_t z = 0; b_bytes(&b, &z, 1); }
    b_patch_u32(&b, avih + 0,  UPF);
    b_patch_u32(&b, avih + 16, 4);
    b_patch_u32(&b, avih + 32, 320);
    b_patch_u32(&b, avih + 36, 240);
    b_patch_u32(&b, hdrl_size_at, (uint32_t)(b.n - hdrl_body));

    b_fourcc(&b, "LIST");
    size_t movi_size_at = b.n; b_u32(&b, 0);
    size_t movi_body = b.n;
    b_fourcc(&b, "movi");
    for (int f = 0; f < 4; f++) {
        uint8_t pay[8] = { 0xFF, 0xD8, (uint8_t)f, 0, 0, 0, 0, 0 };
        b_chunk(&b, "00dc", pay, sizeof pay);
    }
    b_patch_u32(&b, movi_size_at, (uint32_t)(b.n - movi_body));

    b_fourcc(&b, "idx1");
    size_t idx_size_at = b.n; b_u32(&b, 0);
    size_t idx_body = b.n;
    b_fourcc(&b, "??xx");                              /* id nothing will match */
    b_u32(&b, 0);
    b_u32(&b, 0xDEADBEEF);                             /* offset points nowhere sane */
    b_u32(&b, 8);
    b_patch_u32(&b, idx_size_at, (uint32_t)(b.n - idx_body));
    b_patch_u32(&b, riff_size_at, (uint32_t)(b.n - 8));

    return flush_to_tmp(&b);
}

/* A clip with a JUNK chunk at the TOP LEVEL (sibling of LIST hdrl / LIST
 * movi, not inside either) — avi_open()'s "skip (LISTs too)" line for
 * non-LIST top-level chunks (avi.c:45) is otherwise never exercised, since
 * build_avi() only ever has the two LISTs back to back. */
static const char *build_avi_with_top_level_junk(void)
{
    buf_t b = {0};
    b_fourcc(&b, "RIFF");
    size_t riff_size_at = b.n; b_u32(&b, 0);
    b_fourcc(&b, "AVI ");

    b_fourcc(&b, "LIST");
    size_t hdrl_size_at = b.n; b_u32(&b, 0);
    size_t hdrl_body = b.n;
    b_fourcc(&b, "hdrl");
    b_fourcc(&b, "avih");
    b_u32(&b, 56);
    size_t avih = b.n;
    for (int i = 0; i < 56; i++) { uint8_t z = 0; b_bytes(&b, &z, 1); }
    b_patch_u32(&b, avih + 0,  UPF);
    b_patch_u32(&b, avih + 16, 1);
    b_patch_u32(&b, avih + 32, 320);
    b_patch_u32(&b, avih + 36, 240);
    b_patch_u32(&b, hdrl_size_at, (uint32_t)(b.n - hdrl_body));

    /* top-level JUNK chunk between hdrl and movi -- not inside a LIST */
    uint8_t junk[10] = {0};
    b_chunk(&b, "JUNK", junk, sizeof junk);

    b_fourcc(&b, "LIST");
    size_t movi_size_at = b.n; b_u32(&b, 0);
    size_t movi_body = b.n;
    b_fourcc(&b, "movi");
    uint8_t pay[8] = { 0xFF, 0xD8, 0, 0, 0, 0, 0, 0 };
    b_chunk(&b, "00dc", pay, sizeof pay);
    b_patch_u32(&b, movi_size_at, (uint32_t)(b.n - movi_body));
    b_patch_u32(&b, riff_size_at, (uint32_t)(b.n - 8));

    return flush_to_tmp(&b);
}

/* Zero-size / oversized chunks inside movi. avi.c's own scope for "bad chunk
 * sizes" is: a zero-size chunk must be skipped without emitting anything (the
 * "empty / dropped frame" comment), and a chunk whose declared size runs past
 * movi_end/EOF must not send avi_next() into a runaway loop or a crash — the
 * caller (video_play.c's pf_step) is the one that rejects sizes outside
 * [2, VIDEO_FRAME_MAX], but the demuxer itself must survive being handed one. */
static const char *build_avi_zero_and_oversized(void)
{
    buf_t b = {0};
    b_fourcc(&b, "RIFF");
    size_t riff_size_at = b.n; b_u32(&b, 0);
    b_fourcc(&b, "AVI ");
    b_fourcc(&b, "LIST");
    size_t hdrl_size_at = b.n; b_u32(&b, 0);
    size_t hdrl_body = b.n;
    b_fourcc(&b, "hdrl");
    b_fourcc(&b, "avih");
    b_u32(&b, 56);
    size_t avih = b.n;
    for (int i = 0; i < 56; i++) { uint8_t z = 0; b_bytes(&b, &z, 1); }
    b_patch_u32(&b, avih + 0,  UPF);
    b_patch_u32(&b, avih + 16, 3);
    b_patch_u32(&b, avih + 32, 320);
    b_patch_u32(&b, avih + 36, 240);
    b_patch_u32(&b, hdrl_size_at, (uint32_t)(b.n - hdrl_body));

    b_fourcc(&b, "LIST");
    size_t movi_size_at = b.n; b_u32(&b, 0);
    size_t movi_body = b.n;
    b_fourcc(&b, "movi");
    /* frame 0: normal */
    uint8_t p0[8] = { 0xFF, 0xD8, 0, 0, 0, 0, 0, 0 };
    b_chunk(&b, "00dc", p0, sizeof p0);
    /* frame 1: zero-size ("empty / dropped frame") -- must be skipped, not
     * returned as a frame and not mistaken for a chunk with real payload. */
    b_chunk(&b, "00dc", (const uint8_t *)"", 0);
    /* frame 2: declares a size that runs past the end of the movi list /
     * file. The payload bytes after it don't exist -- avi_next() must still
     * return it exactly once (the demuxer only reports the size; the caller
     * validates it) and then cleanly reach AVI_END on the next call instead
     * of looping. */
    b_fourcc(&b, "00dc");
    b_u32(&b, 0x7FFFFFFF);
    b_patch_u32(&b, movi_size_at, (uint32_t)(b.n - movi_body));
    b_patch_u32(&b, riff_size_at, (uint32_t)(b.n - 8));

    return flush_to_tmp(&b);
}

/* A chunk (e.g. a "strl" stream-header LIST, common in real AVIs) INSIDE
 * hdrl, BEFORE avih -- parse_hdrl()'s own skip line (avi.c:45) is only
 * reached when the FIRST chunk it looks at isn't avih, which none of the
 * builders above ever do (avih is always first). */
static const char *build_avi_with_hdrl_junk(void)
{
    buf_t b = {0};
    b_fourcc(&b, "RIFF");
    size_t riff_size_at = b.n; b_u32(&b, 0);
    b_fourcc(&b, "AVI ");

    b_fourcc(&b, "LIST");
    size_t hdrl_size_at = b.n; b_u32(&b, 0);
    size_t hdrl_body = b.n;
    b_fourcc(&b, "hdrl");
    uint8_t junk[6] = {0};
    b_chunk(&b, "JUNK", junk, sizeof junk);   /* parse_hdrl() must skip this to find avih */
    b_fourcc(&b, "avih");
    b_u32(&b, 56);
    size_t avih = b.n;
    for (int i = 0; i < 56; i++) { uint8_t z = 0; b_bytes(&b, &z, 1); }
    b_patch_u32(&b, avih + 0,  UPF);
    b_patch_u32(&b, avih + 16, 1);
    b_patch_u32(&b, avih + 32, 320);
    b_patch_u32(&b, avih + 36, 240);
    b_patch_u32(&b, hdrl_size_at, (uint32_t)(b.n - hdrl_body));

    b_fourcc(&b, "LIST");
    size_t movi_size_at = b.n; b_u32(&b, 0);
    size_t movi_body = b.n;
    b_fourcc(&b, "movi");
    uint8_t pay[8] = { 0xFF, 0xD8, 0, 0, 0, 0, 0, 0 };
    b_chunk(&b, "00dc", pay, sizeof pay);
    b_patch_u32(&b, movi_size_at, (uint32_t)(b.n - movi_body));
    b_patch_u32(&b, riff_size_at, (uint32_t)(b.n - 8));

    return flush_to_tmp(&b);
}

/* A 'rec ' LIST inside movi, wrapping one video chunk -- some AVI muxers
 * interleave audio/video chunks inside a nested LIST per "record". avi_next()
 * must descend into it (avi.c:151-153) instead of treating it as an unknown
 * chunk type to skip. */
static const char *build_avi_with_rec_list(void)
{
    buf_t b = {0};
    b_fourcc(&b, "RIFF");
    size_t riff_size_at = b.n; b_u32(&b, 0);
    b_fourcc(&b, "AVI ");
    b_fourcc(&b, "LIST");
    size_t hdrl_size_at = b.n; b_u32(&b, 0);
    size_t hdrl_body = b.n;
    b_fourcc(&b, "hdrl");
    b_fourcc(&b, "avih");
    b_u32(&b, 56);
    size_t avih = b.n;
    for (int i = 0; i < 56; i++) { uint8_t z = 0; b_bytes(&b, &z, 1); }
    b_patch_u32(&b, avih + 0,  UPF);
    b_patch_u32(&b, avih + 16, 1);
    b_patch_u32(&b, avih + 32, 320);
    b_patch_u32(&b, avih + 36, 240);
    b_patch_u32(&b, hdrl_size_at, (uint32_t)(b.n - hdrl_body));

    b_fourcc(&b, "LIST");
    size_t movi_size_at = b.n; b_u32(&b, 0);
    size_t movi_body = b.n;
    b_fourcc(&b, "movi");

    b_fourcc(&b, "LIST");                       /* the 'rec ' grouping */
    size_t rec_size_at = b.n; b_u32(&b, 0);
    size_t rec_body = b.n;
    b_fourcc(&b, "rec ");
    uint8_t pay[8] = { 0xFF, 0xD8, 0x2A, 0, 0, 0, 0, 0 };   /* 0x2A tags this frame for the test */
    b_chunk(&b, "00dc", pay, sizeof pay);
    b_patch_u32(&b, rec_size_at, (uint32_t)(b.n - rec_body));

    b_patch_u32(&b, movi_size_at, (uint32_t)(b.n - movi_body));
    b_patch_u32(&b, riff_size_at, (uint32_t)(b.n - 8));

    return flush_to_tmp(&b);
}

/* Same as build_avi_with_idx1(), but with a JUNK chunk between movi and idx1
 * -- avi_build_index()'s own top-level scan for idx1 (avi.c:203-205) must
 * skip past it, not just succeed when idx1 happens to be the very next
 * chunk (which is all build_avi_with_idx1() exercises). */
static const char *build_avi_with_idx1_after_padding(void)
{
    buf_t b = {0};
    b_fourcc(&b, "RIFF");
    size_t riff_size_at = b.n; b_u32(&b, 0);
    b_fourcc(&b, "AVI ");
    b_fourcc(&b, "LIST");
    size_t hdrl_size_at = b.n; b_u32(&b, 0);
    size_t hdrl_body = b.n;
    b_fourcc(&b, "hdrl");
    b_fourcc(&b, "avih");
    b_u32(&b, 56);
    size_t avih = b.n;
    for (int i = 0; i < 56; i++) { uint8_t z = 0; b_bytes(&b, &z, 1); }
    b_patch_u32(&b, avih + 0,  UPF);
    b_patch_u32(&b, avih + 16, 4);
    b_patch_u32(&b, avih + 32, 320);
    b_patch_u32(&b, avih + 36, 240);
    b_patch_u32(&b, hdrl_size_at, (uint32_t)(b.n - hdrl_body));

    b_fourcc(&b, "LIST");
    size_t movi_size_at = b.n; b_u32(&b, 0);
    size_t movi_body = b.n;
    long movi_fourcc_pos = (long)movi_body;
    b_fourcc(&b, "movi");
    static long chunk_pos[4];
    for (int f = 0; f < 4; f++) {
        chunk_pos[f] = (long)b.n;
        uint8_t pay[8] = { 0xFF, 0xD8, (uint8_t)f, 0, 0, 0, 0, 0 };
        b_chunk(&b, "00dc", pay, sizeof pay);
    }
    b_patch_u32(&b, movi_size_at, (uint32_t)(b.n - movi_body));

    uint8_t padding[20] = {0};                  /* avi_build_index() must skip over this */
    b_chunk(&b, "JUNK", padding, sizeof padding);

    b_fourcc(&b, "idx1");
    size_t idx_size_at = b.n; b_u32(&b, 0);
    size_t idx_body = b.n;
    for (int f = 0; f < 4; f++) {
        b_fourcc(&b, "00dc");
        b_u32(&b, 0x10);
        b_u32(&b, (uint32_t)(chunk_pos[f] - movi_fourcc_pos));
        b_u32(&b, 8);
    }
    b_patch_u32(&b, idx_size_at, (uint32_t)(b.n - idx_body));
    b_patch_u32(&b, riff_size_at, (uint32_t)(b.n - 8));

    return flush_to_tmp(&b);
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

/* Write raw bytes to a fresh temp file (for the malformed-header cases below,
 * which are shorter than a real RIFF walk needs and don't benefit from the
 * chunk-builder helpers). */
static const char *write_raw(const void *p, size_t n)
{
    char *path = strdup("/tmp/mtest/test_avi_XXXXXX");   /* see flush_to_tmp() re: heap vs static */
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    if (write(fd, p, n) != (ssize_t)n) { perror("write"); exit(2); }
    close(fd);
    return path;
}

/* Every way avi_open() reaches its `fail:` label on a file that DID open
 * (avi.c:106-108, otherwise uncovered -- test_bad_open above only exercises
 * fopen() itself failing). */
static void test_bad_signatures(void)
{
    avi_t a;

    const char *not_riff = write_raw("NOTRIFF-JUNK", 12);
    CHECK(!avi_open(&a, not_riff, NULL, 0), "missing RIFF magic fails");
    unlink(not_riff);

    uint8_t not_avi[12];
    memcpy(not_avi, "RIFF", 4);
    memset(not_avi + 4, 0, 4);
    memcpy(not_avi + 8, "WAVE", 4);           /* valid RIFF, wrong form type */
    const char *wave = write_raw(not_avi, sizeof not_avi);
    CHECK(!avi_open(&a, wave, NULL, 0), "RIFF present but form type != AVI fails");
    unlink(wave);

    /* RIFF/AVI with a well-formed hdrl but NO movi list anywhere -- the
     * top-level walk runs out (pos+8 > file_end) and falls through to
     * `fail:` without ever hitting `return true`. */
    buf_t b = {0};
    b_fourcc(&b, "RIFF");
    size_t riff_size_at = b.n; b_u32(&b, 0);
    b_fourcc(&b, "AVI ");
    b_fourcc(&b, "LIST");
    size_t hdrl_size_at = b.n; b_u32(&b, 0);
    size_t hdrl_body = b.n;
    b_fourcc(&b, "hdrl");
    b_fourcc(&b, "avih");
    b_u32(&b, 56);
    for (int i = 0; i < 56; i++) { uint8_t z = 0; b_bytes(&b, &z, 1); }
    b_patch_u32(&b, hdrl_size_at, (uint32_t)(b.n - hdrl_body));
    b_patch_u32(&b, riff_size_at, (uint32_t)(b.n - 8));
    const char *no_movi = flush_to_tmp(&b);
    CHECK(!avi_open(&a, no_movi, NULL, 0), "well-formed hdrl but no movi list fails");
    unlink(no_movi);

    /* A top-level chunk that declares a size of 0 makes NO progress (next ==
     * pos): avi_open()'s "malformed: no progress" guard must fail cleanly
     * rather than spin forever. */
    buf_t b2 = {0};
    b_fourcc(&b2, "RIFF");
    size_t riff_size_at2 = b2.n; b_u32(&b2, 0);
    b_fourcc(&b2, "AVI ");
    b_fourcc(&b2, "JUNK");
    b_u32(&b2, 0);                             /* declared size 0 -> next == pos */
    b_patch_u32(&b2, riff_size_at2, (uint32_t)(b2.n - 8));
    const char *no_progress = flush_to_tmp(&b2);
    CHECK(!avi_open(&a, no_progress, NULL, 0), "zero-size top-level chunk (no progress) fails");
    unlink(no_progress);

    OK("avi_open() fail: paths (bad RIFF / bad form-type / no movi / no-progress)");
}

/* Stale FILE* self-heal: on-device an SD sleep/wake kills the persistent
 * handle out from under the demuxer. avi_read() and avi_next() both retry
 * with a fresh fopen() of the SAME path (avi_reopen(), avi.c:114-121 --
 * entirely uncovered otherwise) and resume exactly where they left off.
 *
 * Simulating "the OS invalidated this handle behind our back" needs care:
 * fclose(a.f) FREES the FILE object (glibc), so any further use is a
 * use-after-free that can crash on unrelated heap corruption rather than
 * fail the way a dead fd does. close(fileno(a.f)) instead kills only the
 * underlying fd and leaves the FILE* struct itself intact -- the next
 * fread()/fseek() gets a real EBADF from the kernel, exactly like a read
 * against a remounted SD card, without touching freed memory. Buffering has
 * to be turned off too: these test files are a few hundred bytes, well under
 * stdio's default ~4KB buffer, so the FIRST read could silently slurp the
 * whole file and every later "read" would be served from that buffer with no
 * syscall at all -- closing the fd would then prove nothing. */
static void test_reopen_self_heal(const char *path)
{
    avi_t a;
    CHECK(avi_open(&a, path, NULL, 0), "open for the avi_read() self-heal test");
    setvbuf(a.f, NULL, _IONBF, 0);              /* force every I/O op to hit the real fd */
    long sz;
    CHECK(avi_next(&a, &sz) == AVI_VIDEO, "first chunk is frame 0's video");

    close(fileno(a.f));                         /* kill the fd, leave the FILE* struct alive */
    uint8_t p[64] = {0};
    size_t got = avi_read(&a, p, (size_t)sz);
    CHECK(got == (size_t)sz, "avi_read() self-heals a stale handle and finishes the read");
    CHECK(p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0, "self-healed read returns the RIGHT bytes");
    CHECK(a.f != NULL, "a->f is a fresh, valid handle after self-heal");
    avi_close(&a);
    OK("avi_read() self-heals a stale FILE* (avi_reopen)");

    /* Same failure, but hit inside avi_next()'s own fseek/rd_fourcc instead
     * of avi_read() -- a DIFFERENT call site into avi_reopen() (avi.c:145). */
    avi_t a2;
    CHECK(avi_open(&a2, path, NULL, 0), "open for the avi_next() self-heal test");
    setvbuf(a2.f, NULL, _IONBF, 0);
    CHECK(avi_next(&a2, &sz) == AVI_VIDEO, "frame 0 read (advances movi_pos)");
    uint8_t tmp[64];
    avi_read(&a2, tmp, (size_t)sz);             /* consume it so movi_pos points at frame 0's audio chunk */

    close(fileno(a2.f));                        /* stale again, now positioned mid-movi */
    avi_kind_t k = avi_next(&a2, &sz);
    CHECK(k == AVI_AUDIO, "avi_next() self-heals and correctly reads the NEXT chunk (the audio one)");
    CHECK(a2.f != NULL, "a2->f is fresh after avi_next()'s self-heal");
    avi_close(&a2);
    OK("avi_next() self-heals a stale FILE* mid-walk (avi_reopen, 2nd call site)");
}

/* avi_build_index(): idx1 pre-fill lets a big forward seek land near the
 * target in one jump instead of walking every chunk from the last playback
 * checkpoint. Verified by correctness (lands on the right frame) since gcov
 * coverage alone can't tell "used the index" from "walked anyway". */
static void test_idx1_seek(void)
{
    const char *path = build_avi_with_idx1();
    avi_t a;
    CHECK(avi_open(&a, path, NULL, 0), "open the idx1-carrying clip");
    CHECK(!a.indexed, "idx1 not scanned yet -- lazy, only on the first seek");

    /* A big forward jump with NO playback checkpoints recorded yet (no
     * avi_next() calls at all): only the idx1 pre-fill can make this land
     * near frame 250 instead of falling back to movi_start. */
    avi_seek_frame(&a, 250);
    CHECK(a.indexed, "first seek triggers the lazy idx1 scan");
    /* Prove the INDEX was actually used, not just that the destination was
     * reached (a brute-force walk-from-ckpt[0] fallback would also land on
     * the right frame, just slower -- gcov can't distinguish the two, only
     * this can): a mid-range checkpoint the lazy scan could only have filled
     * from idx1 (no avi_next() has run yet to record it from playback). */
    bool any_midrange_ckpt = false;
    for (int k = 1; k < AVI_CKPT_N; k++) if (a.ckpt[k] != 0) { any_midrange_ckpt = true; break; }
    CHECK(any_midrange_ckpt, "idx1 scan actually populated ckpt[] (not just a lucky fallback walk)");
    long sz;
    CHECK(avi_next(&a, &sz) == AVI_VIDEO, "post-seek chunk is a video frame");
    uint8_t p[16] = {0};
    avi_read(&a, p, (size_t)sz);
    int frame = p[2] | (p[3] << 8);
    CHECK(frame == 250, "idx1-assisted forward seek lands exactly on frame 250");

    avi_seek_frame(&a, 10);
    CHECK(avi_next(&a, &sz) == AVI_VIDEO, "post-seek chunk is a video frame (backward)");
    memset(p, 0, sizeof p);
    avi_read(&a, p, (size_t)sz);
    frame = p[2] | (p[3] << 8);
    CHECK(frame == 10, "idx1-assisted backward seek lands exactly on frame 10");

    avi_close(&a);
    unlink(path);
    OK("avi_build_index() idx1 pre-fill drives an exact-landing seek");
}

/* A corrupted / unrecognisable idx1 (avi.c:210-226's `have_base` loop):
 * neither offset-base candidate confirms, so avi_build_index() must leave
 * ckpt[] exactly as recorded during playback instead of trusting garbage
 * offsets -- seeking still works, just via the walk-from-checkpoint path. */
static void test_idx1_unrecognised(void)
{
    const char *path = build_avi_with_bad_idx1();
    avi_t a;
    CHECK(avi_open(&a, path, NULL, 0), "open the bad-idx1 clip");
    long sz;
    while (avi_next(&a, &sz) != AVI_END) {      /* walk it once so ckpt[] gets recorded */
        uint8_t p[16] = {0};
        avi_read(&a, p, (size_t)sz);
    }
    avi_seek_frame(&a, 1);
    CHECK(a.indexed, "unrecognisable idx1 is still marked scanned (tried once)");
    CHECK(avi_next(&a, &sz) == AVI_VIDEO, "seek still works via playback checkpoints");
    avi_close(&a);
    unlink(path);
    OK("unrecognisable idx1 offsets don't corrupt ckpt[] -- seek still works");
}

/* Top-level non-LIST chunk between hdrl and movi (avi.c:45's "skip (LISTs
 * too)" -- never exercised by build_avi(), whose two LISTs are adjacent). */
static void test_top_level_junk_skip(void)
{
    const char *path = build_avi_with_top_level_junk();
    avi_t a;
    CHECK(avi_open(&a, path, NULL, 0), "open a clip with a top-level JUNK chunk before movi");
    CHECK(a.width == 320 && a.height == 240, "header still parsed correctly");
    long sz;
    CHECK(avi_next(&a, &sz) == AVI_VIDEO, "movi is still found and walked after the JUNK chunk");
    avi_close(&a);
    unlink(path);
    OK("top-level JUNK chunk between hdrl and movi is skipped, not misread as a LIST");
}

/* Zero-size ("empty / dropped frame") chunks are skipped outright; an
 * oversized declared chunk size is handed back once (the caller validates
 * it, not the demuxer) and the demuxer still reaches AVI_END afterwards
 * instead of looping or reading off the end of the file. */
static void test_zero_and_oversized_chunks(void)
{
    const char *path = build_avi_zero_and_oversized();
    avi_t a;
    CHECK(avi_open(&a, path, NULL, 0), "open the zero/oversized-chunk clip");

    long sz;
    CHECK(avi_next(&a, &sz) == AVI_VIDEO, "frame 0 (normal) is returned");
    uint8_t p[16] = {0};
    avi_read(&a, p, (size_t)sz);

    CHECK(avi_next(&a, &sz) == AVI_VIDEO, "frame 2 is returned (frame 1's zero-size chunk was skipped)");
    CHECK(sz == 0x7FFFFFFF, "the oversized chunk's declared size is passed through as-is");

    /* Do NOT avi_read() the oversized chunk -- there is no such payload in
     * the file; a real caller (video_play.c's pf_step) rejects sizes outside
     * [2, VIDEO_FRAME_MAX] before ever reading. What matters here is that the
     * demuxer's OWN cursor survives an oversized size without crashing or
     * looping: the next call must reach AVI_END cleanly. */
    CHECK(avi_next(&a, &sz) == AVI_END, "demuxer reaches AVI_END after an oversized chunk, not a runaway loop");
    CHECK(avi_next(&a, &sz) == AVI_END, "AVI_END is idempotent -- calling again doesn't crash or resurrect a chunk");

    avi_close(&a);
    unlink(path);
    OK("zero-size chunk skipped; oversized chunk size passed through; AVI_END is stable");
}

/* parse_hdrl()'s skip line (avi.c:45): a chunk inside hdrl before avih must
 * be walked past, not mistaken for avih or aborted on. */
static void test_hdrl_skip(void)
{
    const char *path = build_avi_with_hdrl_junk();
    avi_t a;
    CHECK(avi_open(&a, path, NULL, 0), "open a clip with a JUNK chunk inside hdrl, before avih");
    CHECK(a.width == 320 && a.height == 240, "avih still found and parsed after skipping the JUNK chunk");
    CHECK(a.usec_per_frame == (int)UPF, "usec_per_frame still parsed correctly");
    avi_close(&a);
    unlink(path);
    OK("hdrl chunk skip (avih not first) parses correctly");
}

/* avi_next()'s 'rec ' LIST descent (avi.c:151-153). */
static void test_rec_list_descent(void)
{
    const char *path = build_avi_with_rec_list();
    avi_t a;
    CHECK(avi_open(&a, path, NULL, 0), "open a clip whose movi wraps its chunk in a 'rec ' LIST");
    long sz;
    CHECK(avi_next(&a, &sz) == AVI_VIDEO, "avi_next() descends into the 'rec ' LIST and finds the video chunk");
    uint8_t p[16] = {0};
    avi_read(&a, p, (size_t)sz);
    CHECK(p[2] == 0x2A, "the chunk found inside the 'rec ' LIST is the right one");
    avi_close(&a);
    unlink(path);
    OK("avi_next() descends into a 'rec ' grouping LIST instead of skipping it");
}

/* avi_build_index()'s idx1-location scan skipping a non-idx1 chunk first
 * (avi.c:203-205) -- build_avi_with_idx1() alone never reaches this because
 * idx1 is the very next chunk after movi there. */
static void test_idx1_scan_skips_padding(void)
{
    const char *path = build_avi_with_idx1_after_padding();
    avi_t a;
    CHECK(avi_open(&a, path, NULL, 0), "open a clip with a JUNK chunk between movi and idx1");
    avi_seek_frame(&a, 3);
    CHECK(a.indexed, "seek triggers the lazy idx1 scan");
    long sz;
    CHECK(avi_next(&a, &sz) == AVI_VIDEO, "post-seek chunk is a video frame");
    uint8_t p[16] = {0};
    avi_read(&a, p, (size_t)sz);
    CHECK(p[2] == 3, "idx1 scan found the index past the JUNK padding and seek landed correctly");
    avi_close(&a);
    unlink(path);
    OK("avi_build_index()'s idx1 scan skips a non-idx1 chunk before finding it");
}

/* avi_next()'s self-heal giving up (avi.c:146's `break`): if the file itself
 * is gone (not just a dead fd), avi_reopen()'s fopen() fails too, and the
 * demuxer must report AVI_END instead of looping or crashing. */
static void test_reopen_gives_up_when_file_is_gone(void)
{
    const char *path = build_avi();
    avi_t a;
    CHECK(avi_open(&a, path, NULL, 0), "open before deleting the underlying file");
    setvbuf(a.f, NULL, _IONBF, 0);
    long sz;
    CHECK(avi_next(&a, &sz) == AVI_VIDEO, "first chunk read fine while the file still exists");

    close(fileno(a.f));
    unlink(path);                               /* the file is now genuinely gone */
    avi_kind_t k = avi_next(&a, &sz);
    CHECK(k == AVI_END, "avi_next() gives up cleanly (AVI_END) when avi_reopen()'s fopen() also fails");
    avi_close(&a);
    OK("avi_next() reports AVI_END, not a crash or infinite retry, when the file is truly gone");
}

/* avi_reopen()'s setvbuf branch (avi.c:120) only runs when avi_open() was
 * given a real read-ahead buffer -- every test above passes rabuf=NULL. */
static void test_reopen_reapplies_rabuf(void)
{
    const char *path = build_avi();
    /* Deliberately TINY (8 bytes, exactly avi_next()'s own fourcc+size read):
     * with a normal multi-KB rabuf this whole small test file would get
     * slurped into the buffer on the first read, and the later avi_read()
     * would be served from that buffer without ever touching the (killed)
     * fd -- proving nothing about avi_reopen(). A tiny buffer guarantees the
     * chunk-payload read below needs a fresh fill, which is what actually
     * exercises the self-heal path. */
    static uint8_t rabuf[8];
    avi_t a;
    CHECK(avi_open(&a, path, rabuf, sizeof rabuf), "open with a real (tiny) read-ahead buffer");
    long sz;
    CHECK(avi_next(&a, &sz) == AVI_VIDEO, "first chunk read fine with read-ahead buffering");

    close(fileno(a.f));                         /* stale handle -> avi_reopen() re-applies rabuf */
    uint8_t p[64] = {0};
    size_t got = avi_read(&a, p, (size_t)sz);
    CHECK(got == (size_t)sz, "self-heal with a read-ahead buffer still completes the read");
    CHECK(p[0] == 0xFF && p[1] == 0xD8, "self-healed (rabuf) read returns the right bytes");
    avi_close(&a);
    unlink(path);
    OK("avi_reopen() re-applies the caller's read-ahead buffer after a stale handle");
}

int main(void)
{
    printf("== test_avi ==\n");
    const char *path = build_avi();
    test_header_parse(path);
    test_walk_and_read(path);
    test_seek(path);
    test_bad_open();
    test_bad_signatures();
    test_reopen_self_heal(path);
    test_idx1_seek();
    test_idx1_unrecognised();
    test_top_level_junk_skip();
    test_zero_and_oversized_chunks();
    test_hdrl_skip();
    test_rec_list_descent();
    test_idx1_scan_skips_padding();
    test_reopen_gives_up_when_file_is_gone();
    test_reopen_reapplies_rabuf();
    unlink(path);

    if (g_failures) { printf("FAILED (%d)\n", g_failures); return 1; }
    printf("all avi tests passed\n");
    return 0;
}
