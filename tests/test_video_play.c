/* Host unit tests for Core/Src/porting/video/video_play.c's prefetch state
 * machine: pf_reset() / pf_step() / pf_fetch() (static, so this file
 * #includes video_play.c directly — same pattern tests/test_clock_alarm.c
 * uses for rg_clock.c). This is the jitter buffer that reads upcoming video
 * frames / feeds audio during the pacing wait so a busy scene's oversized
 * frame is already in RAM when its turn comes, and it holds the mechanism
 * that jammed shut during the ring-drift bug ("Nothing synchronises the two
 * clocks" in this dir's CLAUDE.md): once the audio ring got full, the gate in
 * pf_step() never opened again and every frame read became a blocking one.
 *
 * video_play.c pulls in nearly the whole porting-layer surface (LCD, audio,
 * input, overlay, i18n, alarm) even though pf_step/pf_fetch/pf_reset touch
 * only avi.c + video_audio.c's public API. Everything below EXCEPT those two
 * is a hardware seam (LCD/SAI/input/overlay peripherals) or UI plumbing this
 * file's tests never exercise but the compiler still needs a body for,
 * because video_play.c is one translation unit and video_play() itself
 * (never called here) still references all of it. avi.c, video_decode.c and
 * video_audio.c (+ the real minimp3 decoder) are linked in for REAL — this
 * is the demuxer and the audio ring the prefetcher actually drives, not a
 * reimplementation of either.
 *
 * Compile + run (also in tests/run.sh / tests/coverage.sh):
 *   gcc -O2 -Wall -Wextra -std=gnu11 -Itests/video_stubs -ICore/Inc/porting/video \
 *       -ICore/Src/porting/lib -ICore/Inc/porting/music \
 *       tests/test_video_play.c Core/Src/porting/video/avi.c \
 *       Core/Src/porting/video/video_decode.c Core/Src/porting/video/video_audio.c \
 *       Core/Src/porting/music/music_minimp3.c -o /tmp/mtest/test_video_play
 */
/* strdup()/mkstemp()/fileno() are POSIX, not standard C -- see tests/test_avi.c's
 * identical comment for why this matters under a strict -std=c11 build. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

/* Pull in the stub type definitions BEFORE using them below -- video_play.c
 * itself includes all of these too (further down, once #included), but this
 * test's own stub bodies need the types first. */
#include "common.h"
#include "odroid_input.h"
#include "odroid_overlay.h"
#include "rg_i18n.h"
#include "gui.h"
#include "minimp3.h"

/* ---- hardware / UI seams --------------------------------------------------
 * video_play.c is compiled whole (its statics are only reachable that way),
 * so every extern it references anywhere in the file -- including inside
 * video_play() itself, never called by these tests -- needs a link-time body.
 * None of these are exercised by the prefetch tests below; they only have to
 * exist. */
static uint32_t s_fake_tick = 1;
uint32_t HAL_GetTick(void) { return s_fake_tick++; }   /* always advances: dt across any I/O is > 0 */
void HAL_Delay(uint32_t ms) { s_fake_tick += ms; }
void wdog_refresh(void) {}
void SCB_CleanDCache_by_Addr(uint32_t *addr, int32_t dsize) { (void)addr; (void)dsize; }

static uint16_t s_fb[320 * 240];
uint16_t *lcd_get_active_buffer(void) { return s_fb; }
void lcd_swap(void) {}

void music_attach(int16_t *ring, int size, volatile uint16_t *head, volatile uint16_t *tail)
{ (void)ring; (void)size; (void)head; (void)tail; }
void music_audio_enable(int on) { (void)on; }
void music_audio_set(int vol, int play) { (void)vol; (void)play; }
void audio_start_playing(uint16_t length) { (void)length; }
void audio_stop_playing(void) {}

uint8_t common_emu_sound_get_volume(void) { return 8; }
common_emu_state_t common_emu_state;

uint32_t JPEG_DecodeToFrameInit(uint32_t buf, uint32_t sz) { (void)buf; (void)sz; return 0; }
uint32_t JPEG_DecodeDeInit(void) { return 0; }
uint32_t JPEG_DecodeToFrame(uint32_t src, uint32_t sz, uint32_t dst, uint16_t x, uint16_t y, uint8_t la)
{ (void)src; (void)sz; (void)dst; (void)x; (void)y; (void)la; return 0; }

#define SCRATCH_MAX (352 * 1024)
uint8_t g_scratch[SCRATCH_MAX];

int odroid_audio_volume_get(void) { return 5; }
void odroid_audio_volume_set(int level) { (void)level; }

void odroid_input_read_gamepad(odroid_gamepad_state_t *s) { memset(s, 0, sizeof *s); }

int odroid_overlay_settings_menu(odroid_dialog_choice_t *extra, void_callback_t repaint, odroid_menu_flags_t flags)
{ (void)extra; (void)repaint; (void)flags; return -1; }

static const lang_t s_lang = { .s_info = "Info", .s_Quit_to_menu = "Quit to menu" };
const lang_t *curr_lang = &s_lang;
int i18n_draw_text_line(uint16_t x, uint16_t y, uint16_t w, const char *t, uint16_t c, uint16_t bg, char f)
{ (void)x; (void)y; (void)w; (void)t; (void)c; (void)bg; (void)f; return 0; }
int i18n_get_text_width(const char *t) { return (int)strlen(t) * 6; }

static colors_t s_colors = { 0x0000, 0xFFFF, 0xF800, 0x7BEF };
colors_t *curr_colors = &s_colors;

bool rg_alarm_poll(void) { return false; }

/* video_play.c's own diagnostics globals for the HW JPEG path (declared
 * `extern` in video_play.c itself, not via a header). */
uint32_t g_jpeg_hal, g_jpeg_err, g_jpeg_rej, g_jpeg_sub, g_jpeg_need;

#include "../Core/Src/porting/video/video_play.c"

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

/* ---- AVI fixture builder (same shape as tests/test_avi.c's) -------------- */
typedef struct { uint8_t *p; size_t n, cap; } buf_t;
static void b_need(buf_t *b, size_t extra)
{
    if (b->n + extra <= b->cap) return;
    while (b->cap < b->n + extra) b->cap = b->cap ? b->cap * 2 : 256;
    b->p = realloc(b->p, b->cap);
    if (!b->p) { perror("realloc"); exit(2); }
}
static void b_bytes(buf_t *b, const void *src, size_t n) { b_need(b, n); memcpy(b->p + b->n, src, n); b->n += n; }
static void b_fourcc(buf_t *b, const char *cc) { b_bytes(b, cc, 4); }
static void b_u32(buf_t *b, uint32_t v)
{
    uint8_t e[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    b_bytes(b, e, 4);
}
static void b_patch_u32(buf_t *b, size_t at, uint32_t v)
{
    b->p[at] = (uint8_t)v; b->p[at+1] = (uint8_t)(v>>8); b->p[at+2] = (uint8_t)(v>>16); b->p[at+3] = (uint8_t)(v>>24);
}
static void b_chunk(buf_t *b, const char *cc, const uint8_t *payload, uint32_t len)
{
    b_fourcc(b, cc); b_u32(b, len); b_bytes(b, payload, len);
    if (len & 1) { uint8_t z = 0; b_bytes(b, &z, 1); }
}

#define UPF  41708u
#define NFRAMES 6

/* A clip with NFRAMES tiny "video" chunks (not real JPEGs -- the prefetch
 * machinery under test here never decodes them, it just reads bytes into
 * slots) each carrying a distinct marker byte, plus one audio chunk. */
static const char *build_clip(void)
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
    b_patch_u32(&b, avih + 16, NFRAMES);
    b_patch_u32(&b, avih + 32, 320);
    b_patch_u32(&b, avih + 36, 240);
    b_patch_u32(&b, hdrl_size_at, (uint32_t)(b.n - hdrl_body));

    b_fourcc(&b, "LIST");
    size_t movi_size_at = b.n; b_u32(&b, 0);
    size_t movi_body = b.n;
    b_fourcc(&b, "movi");
    for (int f = 0; f < NFRAMES; f++) {
        uint8_t pay[32];
        memset(pay, (uint8_t)(0x50 + f), sizeof pay);   /* frame f is filled with byte 0x50+f */
        b_chunk(&b, "00dc", pay, sizeof pay);
        /* the audio chunk sits right after frame 0 -- WITHIN the PF_DEPTH=2
         * window the prefetch test below walks (frame 0 then frame 1); one
         * any later than that is never reached before the prefetch queue
         * fills up and the "queue only PF_DEPTH frames ahead" gate stops it. */
        if (f == 0) {
            uint8_t au[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
            b_chunk(&b, "01wb", au, sizeof au);
        }
    }
    b_patch_u32(&b, movi_size_at, (uint32_t)(b.n - movi_body));
    b_patch_u32(&b, riff_size_at, (uint32_t)(b.n - 8));

    char *path = strdup("/tmp/mtest/test_vp_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    if (write(fd, b.p, b.n) != (ssize_t)b.n) { perror("write"); exit(2); }
    close(fd);
    free(b.p);
    return path;
}

/* --------------------------------------------------------------------------
 * pf_reset(): every piece of prefetch state goes back to empty/idle.
 * -------------------------------------------------------------------------- */
static void test_pf_reset(void)
{
    pf_n = 3; pf_busy = 0x7; pf_ip_want = 99; pf_src_end = true;
    pf_reset();
    CHECK(pf_n == 0, "pf_reset: pf_n cleared");
    CHECK(pf_busy == 0, "pf_reset: no slots marked busy");
    CHECK(pf_ip_want == -1, "pf_reset: no read in progress");
    CHECK(pf_src_end == false, "pf_reset: source-end flag cleared");
    OK("pf_reset() clears every field");
}

/* --------------------------------------------------------------------------
 * pf_step()/pf_fetch(): a burst frame is already in RAM when its turn comes.
 * Running pf_step(force=false) ahead of time (as the pacing wait does) queues
 * PF_DEPTH frames into slots; pf_fetch() then hands one out WITHOUT any
 * blocking read (g_vdec_read_ms stays 0 across the call) -- that's the whole
 * point of the jitter buffer.
 * -------------------------------------------------------------------------- */
static void test_prefetch_then_fetch_no_blocking_read(void)
{
    const char *path = build_clip();
    avi_t a;
    CHECK(avi_open(&a, path, NULL, 0), "open the synthetic clip");
    pf_reset();
    int na = 0;

    /* Run the non-blocking prefetch step until it has nothing more to do
     * this "tick" -- exactly what the pacing-wait loop in video_play() does. */
    int guard = 0;
    while (pf_step(&a, 1, false, &na, false) && guard++ < 100) {}
    CHECK(pf_n == PF_DEPTH, "prefetch queued PF_DEPTH frames ahead while idle");
    CHECK(na == 1, "the one interleaved audio chunk was consumed along the way");

    g_vdec_read_ms = 12345;      /* poison it; pf_fetch() must NOT touch this for a queued frame */
    pf_ent_t ent;
    CHECK(pf_fetch(&a, &ent, 1, false, &na), "pf_fetch returns the queued frame 0");
    CHECK(g_vdec_read_ms == 0, "pf_fetch() resets read_ms and does NOT add a blocking read for a prefetched frame");
    CHECK(ent.slot >= 0, "frame 0 landed in a real slot");
    CHECK(video_slot(ent.slot)[0] == 0x50, "the slot holds frame 0's actual bytes (burst frame already in RAM)");

    CHECK(pf_fetch(&a, &ent, 1, false, &na), "pf_fetch returns the queued frame 1");
    CHECK(video_slot(ent.slot)[0] == 0x51, "frame 1's bytes are correct too");

    avi_close(&a);
    unlink(path);
    OK("pf_step() prefetches ahead; pf_fetch() hands frames out with zero blocking read");
}

/* --------------------------------------------------------------------------
 * rd= vs pf= attribution: a FORCED read (consumer starving, force=true) is
 * charged to g_vdec_read_ms; a read that happens during the pacing wait
 * (force=false) is charged to g_vdec_pf_ms. Mixing these up silently lies to
 * the on-screen HUD about whether the prefetch is actually keeping up.
 * -------------------------------------------------------------------------- */
static void test_rd_vs_pf_attribution(void)
{
    const char *path = build_clip();
    avi_t a;
    CHECK(avi_open(&a, path, NULL, 0), "open for the rd=/pf= attribution test");
    pf_reset();
    int na = 0;

    g_vdec_read_ms = 0; g_vdec_pf_ms = 0;
    CHECK(pf_fetch(&a, &(pf_ent_t){0}, 1, false, &na), "pf_fetch with nothing queued forces a blocking read");
    CHECK(g_vdec_read_ms > 0, "a forced (consumer-starving) read is charged to rd= (g_vdec_read_ms)");
    CHECK(g_vdec_pf_ms == 0, "...and NOT to pf= (g_vdec_pf_ms)");
    avi_close(&a);

    /* Fresh avi_t (own cursor, back at frame 0) for the non-forced half --
     * reusing the one above would resume mid-clip, right where the previous
     * check left off (the audio chunk after frame 0), which completes via a
     * different branch of pf_step() entirely and touches neither counter. */
    avi_t a2;
    CHECK(avi_open(&a2, path, NULL, 0), "reopen for the non-forced half");
    pf_reset();
    g_vdec_read_ms = 0; g_vdec_pf_ms = 0;
    /* force=false, called directly the way the pacing-wait loop does it. A
     * video chunk's read is two calls: the first only sets up pf_ip_want
     * from avi_next() (no bytes read yet, so no time to charge); the second
     * actually reads -- matching pf_step()'s own "continue the in-progress
     * frame" branch, which is what timestamps rd=/pf= in the first place. */
    int guard = 0;
    while (g_vdec_pf_ms == 0 && guard++ < 5) pf_step(&a2, 1, false, &na, false);
    CHECK(g_vdec_pf_ms > 0, "a non-forced (pacing-wait) read is charged to pf= (g_vdec_pf_ms)");
    CHECK(g_vdec_read_ms == 0, "...and NOT to rd= (g_vdec_read_ms)");

    avi_close(&a2);
    unlink(path);
    OK("rd= (forced) and pf= (wait-time) reads are attributed to the right counter");
}

/* --------------------------------------------------------------------------
 * The audio-ring gate: this is the mechanism the drift bug jammed shut. Once
 * video_audio_ring_free() < PF_AUDIO_HEADROOM, a non-forced pf_step() must
 * make NO progress on the demuxer at all (not even a partial read) -- it has
 * to hold position until the ring drains, or the prefetcher would keep
 * running ahead of audio it can't queue.
 * -------------------------------------------------------------------------- */
#define MP3_PATH "/tmp/mtest/video_audio_test.mp3"

static void test_audio_ring_gate_closes_prefetch(void)
{
    FILE *f = fopen(MP3_PATH, "rb");
    if (!f) {
        printf("SKIP audio-ring-gate test: %s not present "
               "(tests/run.sh generates it with ffmpeg; ffmpeg missing here)\n", MP3_PATH);
        return;
    }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    uint8_t *mp3 = malloc((size_t)n);
    CHECK(mp3 && fread(mp3, 1, (size_t)n, f) == (size_t)n, "load the MP3 fixture");
    fclose(f);

    /* Fill the REAL audio ring past the gate threshold by decoding real MP3
     * frames and never draining (nothing here plays it back) -- exactly
     * "video_audio_ring_free() < PF_AUDIO_HEADROOM", the condition pf_step()
     * itself checks. One-frame-at-a-time feeding (boundary-aligned) is the
     * same technique tests/test_video_audio.c uses; see that file's header
     * comment for why arbitrary byte windows don't decode reliably. */
    video_audio_start();
    mp3dec_t scratch; mp3dec_init(&scratch);
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    long pos = 0;
    while (pos < n && video_audio_ring_free() >= PF_AUDIO_HEADROOM) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&scratch, mp3 + pos, (int)(n - pos), pcm, &info);
        if (info.frame_bytes == 0) break;
        if (samples > 0) video_audio_feed(mp3 + pos, info.frame_bytes);
        pos += info.frame_bytes;
    }
    CHECK(video_audio_ring_free() < PF_AUDIO_HEADROOM, "test setup: the ring is actually past the gate threshold");

    const char *path = build_clip();
    avi_t a;
    CHECK(avi_open(&a, path, NULL, 0), "open the clip for the gate test");
    pf_reset();
    int na = 0;

    long movi_pos_before = a.movi_pos;
    int  pf_n_before = pf_n;
    /* spd=1, not paused -- the exact condition pf_step() gates on. force
     * MUST be false: the gate explicitly does not apply to the
     * consumer-starving (force=true) path (see pf_step()'s own comment). */
    bool progressed = pf_step(&a, 1, false, &na, false);
    CHECK(!progressed, "pf_step() reports no progress while the audio ring is past the gate");
    CHECK(a.movi_pos == movi_pos_before, "the demuxer's cursor did not move -- avi_next() was never called");
    CHECK(pf_n == pf_n_before, "no frame was queued either");

    avi_close(&a);
    unlink(path);
    free(mp3);
    OK("pf_step()'s audio-ring gate holds the prefetcher still while the ring is past PF_AUDIO_HEADROOM");
}

/* --------------------------------------------------------------------------
 * The gate must NOT apply when force=true (the consumer is actually
 * starving right now and needs the frame regardless of the ring) -- that is
 * exactly the "old synchronous behaviour" pf_step()'s own comment describes.
 * -------------------------------------------------------------------------- */
static void test_forced_step_ignores_the_gate(void)
{
    const char *path = build_clip();
    avi_t a;
    CHECK(avi_open(&a, path, NULL, 0), "open the clip for the forced-ignores-gate test");
    pf_reset();
    int na = 0;

    /* Ring state doesn't matter here -- don't even bother touching it, just
     * confirm force=true makes progress on a fresh demuxer regardless. */
    bool progressed = pf_step(&a, 1, false, &na, true);
    CHECK(progressed, "force=true makes progress even where the gate would otherwise hold");

    avi_close(&a);
    unlink(path);
    OK("force=true (consumer starving) bypasses the audio-ring gate, as documented");
}

int main(void)
{
    printf("== test_video_play ==\n");
    video_decode_init();

    test_pf_reset();
    test_prefetch_then_fetch_no_blocking_read();
    test_rd_vs_pf_attribution();
    test_audio_ring_gate_closes_prefetch();
    test_forced_step_ignores_the_gate();

    video_decode_deinit();

    if (g_failures) { printf("FAILED (%d)\n", g_failures); return 1; }
    printf("all video_play tests passed\n");
    return 0;
}
