// Host unit tests for the AVI demuxer (avi.c). Builds a minimal but valid
// RIFF/AVI in memory (no ffmpeg needed) and checks header parse + movi walk.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "avi.h"

static int checks, fails;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

// --- tiny AVI builder ------------------------------------------------------

static uint8_t B[8192];
static int N;

static void put4(const char *s)            { memcpy(B + N, s, 4); N += 4; }
static void put_u32(uint32_t v)            { B[N++]=v; B[N++]=v>>8; B[N++]=v>>16; B[N++]=v>>24; }
static void put_bytes(const void *p, int n){ if (n) memcpy(B + N, p, n); N += n; }
static void patch_u32(int at, uint32_t v)  { B[at]=v; B[at+1]=v>>8; B[at+2]=v>>16; B[at+3]=v>>24; }

static void add_chunk(const char *id, const void *data, int sz)
{
    put4(id); put_u32((uint32_t)sz);
    put_bytes(data, sz);
    if (sz & 1) B[N++] = 0;                 // word-align (pad byte)
}

static const char *build_avi(void)
{
    static const char *path = "/tmp/t_avi.avi";
    const unsigned char jpg1[] = { 0xFF,0xD8,0xFF,0xE0,0x00,0x10,'J','F','I','F' };  // 10
    const unsigned char jpg2[] = { 0xFF,0xD8,0xFF,0xDB,0,1,2,3,4,5,6,7 };            // 12
    const unsigned char mp3a[] = { 0xFF,0xFB,0x90,0x00,0,0,0,0 };                    // 8
    const unsigned char mp3b[] = { 0xFF,0xFB,0x92,0x00,1,1,1 };                      // 7 (odd -> pad)
    const unsigned char junk[] = { 'x','x','x','x' };

    N = 0;
    put4("RIFF"); int riff_sz = N; put_u32(0); put4("AVI ");

    // hdrl LIST { avih(320x240, 41667 us/frame) }
    put4("LIST"); int hdrl_sz = N; put_u32(0); put4("hdrl");
        put4("avih"); put_u32(56);
        int avih = N;
        put_u32(41667);                      // +0  dwMicroSecPerFrame
        for (int i = 0; i < 7; i++) put_u32(0);  // +4..+28
        put_u32(320);                        // +32 dwWidth
        put_u32(240);                        // +36 dwHeight
        while (N - avih < 56) B[N++] = 0;    // pad rest of the 56-byte header
    patch_u32(hdrl_sz, (uint32_t)(N - (hdrl_sz + 4)));

    // movi LIST { interleaved frames }
    put4("LIST"); int movi_sz = N; put_u32(0); put4("movi");
        add_chunk("00dc", jpg1, sizeof jpg1);   // video 1
        add_chunk("01wb", mp3a, sizeof mp3a);   // audio 1
        add_chunk("00dc", jpg2, sizeof jpg2);   // video 2
        add_chunk("00dc", NULL, 0);             // dropped frame (size 0) -> skipped
        add_chunk("JUNK", junk, sizeof junk);   // junk -> skipped
        add_chunk("00db", jpg1, sizeof jpg1);   // video 3 (db variant)
        add_chunk("01wb", mp3b, sizeof mp3b);   // audio 2 (odd size -> padded)
    patch_u32(movi_sz, (uint32_t)(N - (movi_sz + 4)));

    patch_u32(riff_sz, (uint32_t)(N - (riff_sz + 4)));

    FILE *f = fopen(path, "wb");
    fwrite(B, 1, N, f);
    fclose(f);
    return path;
}

// --- tests -----------------------------------------------------------------

static void test_header(void)
{
    avi_t a;
    CHECK(avi_open(&a, build_avi()), "avi_open succeeds");
    CHECK(a.width == 320, "width 320");
    CHECK(a.height == 240, "height 240");
    CHECK(a.usec_per_frame == 41667, "usec/frame 41667");
    CHECK(avi_frame_ms(&a) == 42, "frame ms == 42 (24fps)");
    avi_close(&a);
}

static void test_walk(void)
{
    avi_t a;
    CHECK(avi_open(&a, build_avi()), "open for walk");

    int nv = 0, na = 0;
    unsigned char v0[2] = {0,0}, a0[2] = {0,0};
    long sz;
    avi_kind_t k;
    while ((k = avi_next(&a, &sz)) != AVI_END) {
        unsigned char hd[2] = {0,0};
        long at = ftell(a.f);
        if (fread(hd, 1, 2, a.f) != 2) hd[0] = 0;
        if (k == AVI_VIDEO) { if (nv == 0) { v0[0]=hd[0]; v0[1]=hd[1]; } nv++; CHECK(sz > 0, "video sz > 0"); }
        if (k == AVI_AUDIO) { if (na == 0) { a0[0]=hd[0]; a0[1]=hd[1]; } na++; }
        (void)at;
    }
    CHECK(nv == 3, "3 video frames (empty + junk skipped)");
    CHECK(na == 2, "2 audio chunks");
    CHECK(v0[0] == 0xFF && v0[1] == 0xD8, "first video is JPEG (FFD8)");
    CHECK(a0[0] == 0xFF && a0[1] == 0xFB, "first audio is MP3 (FFFB)");
    avi_close(&a);
}

static void test_bad(void)
{
    avi_t a;
    FILE *f = fopen("/tmp/t_not.avi", "wb");
    fwrite("NOPEnope", 1, 8, f);
    fclose(f);
    CHECK(!avi_open(&a, "/tmp/t_not.avi"), "non-AVI rejected");
    CHECK(!avi_open(&a, "/tmp/does_not_exist.avi"), "missing file rejected");
}

int main(void)
{
    test_header();
    test_walk();
    test_bad();
    printf("avi: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
