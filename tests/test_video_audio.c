/* Host unit tests for Core/Src/porting/video/video_audio.c — the chunk-fed MP3
 * ring + trim_step() clock-drift servo. See Core/Src/porting/video/CLAUDE.md,
 * "Nothing synchronises the two clocks": the SAI ISR drains the ring at the
 * audio PLL's real rate while the demuxer fills it at SysTick's frame rate;
 * without the servo the mismatch is monotonic and eventually latches the
 * prefetch gate shut (video degrades from smooth to permanently stuttering).
 *
 * video_audio.c is #included directly (not linked) so the static servo state
 * (g_fill_ema, g_step, g_step_base, VR_TARGET, VR_SIZE, ring_count()) is
 * reachable the same way tests/test_clock_alarm.c reaches rg_clock.c's
 * statics. The only hardware seam is music_attach() (registers the ring with
 * the real SAI ISR on-device) — stubbed to capture the pointers, so the test
 * can drain the ring exactly like the ISR would (advance tail).
 *
 * MP3 input: a real file (tests/run.sh / tests/coverage.sh generate it with
 * ffmpeg — 45s of 44.1kHz mono tone at 128kbps) decoded through the REAL
 * minimp3 (Core/Src/porting/music/music_minimp3.c, the firmware's own
 * implementation TU). The servo test feeds it one REAL MP3 frame at a time
 * (frame boundaries found with a throwaway decode pass first): minimp3's
 * mp3dec_decode_frame only trusts a frame without look-ahead confirmation
 * when the given buffer is EXACTLY that frame's length (frame_size ==
 * mp3_bytes skips the "confirm via the next header" check) — feeding
 * arbitrary fixed-size byte windows instead made the decoder discard whole
 * unconfirmed tails and produced no usable samples most calls.
 *
 * Compile + run (also in tests/run.sh / tests/coverage.sh):
 *   gcc -O2 -Wall -Wextra -std=gnu11 -Itests/video_stubs -ICore/Inc/porting/video \
 *       -ICore/Inc/porting/music tests/test_video_audio.c \
 *       Core/Src/porting/music/music_minimp3.c -o /tmp/mtest/test_video_audio
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- hardware seam: music_attach() just hands the SAI ISR the ring pointers.
 * Capture them so the test can drain the ring exactly like the ISR would
 * (advance tail), without reimplementing any of video_audio.c's own logic. */
static volatile uint16_t *g_isr_head;
static volatile uint16_t *g_isr_tail;
void music_attach(int16_t *ring, int size, volatile uint16_t *head, volatile uint16_t *tail)
{
    (void)ring; (void)size;
    g_isr_head = head; g_isr_tail = tail;
}
/* video_play.c's gw_audio.h pulls these in too; video_audio.c itself never
 * calls them, but the header declares them so the link needs a body. */
void music_audio_enable(int on) { (void)on; }
void music_audio_set(int vol, int play) { (void)vol; (void)play; }
void audio_start_playing(uint16_t length) { (void)length; }
void audio_stop_playing(void) {}

#include "../Core/Src/porting/video/video_audio.c"

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

/* Drain `n` samples the way the SAI ISR does (advance g_tail). */
static int isr_drain(int n)
{
    int avail = (*g_isr_head - *g_isr_tail) & VR_MASK;
    if (n > avail) n = avail;
    *g_isr_tail = (uint16_t)((*g_isr_tail + n) & VR_MASK);
    return n;
}

/* --------------------------------------------------------------------------
 * video_audio_start(): ring empty, servo centred on VR_TARGET, no kick at t=0.
 * -------------------------------------------------------------------------- */
static void test_start_state(void)
{
    video_audio_start();
    CHECK(video_audio_ring_count() == 0, "start: ring is empty");
    CHECK(video_audio_ring_free() == VR_SIZE - 1, "start: ring_free is the whole ring");
    CHECK(g_fill_ema == VR_TARGET, "start: g_fill_ema seeded at VR_TARGET (no t=0 kick)");
    CHECK(g_step == g_step_base, "start: g_step starts untrimmed");
    OK("video_audio_start seeds a centred, empty ring");
}

/* --------------------------------------------------------------------------
 * The invariant CLAUDE.md pins: VR_TARGET must stay below the level at which
 * the prefetch gate closes. That level is VR_SIZE-1-PF_AUDIO_HEADROOM, and
 * PF_AUDIO_HEADROOM (2400) lives in video_play.c, not here — the 2400 below
 * is that cross-file constant, written out so a change to either file shows
 * up as a failure here instead of a silent mismatch between the two.
 * -------------------------------------------------------------------------- */
static void test_target_below_gate(void)
{
    const int PF_AUDIO_HEADROOM = 2400;   /* video_play.c's constant, mirrored */
    CHECK(VR_TARGET < VR_SIZE - 1 - PF_AUDIO_HEADROOM,
          "VR_TARGET stays below the prefetch gate's close threshold (1695)");
    OK("VR_TARGET / VR_SIZE / PF_AUDIO_HEADROOM coupling holds");
}

/* --------------------------------------------------------------------------
 * trim_step() direction: an EMA already reading above target must speed the
 * step up (drain faster, matching the comment "consuming input slightly
 * faster ... drains a filling ring"); below target must slow it down. Exactly
 * AT target (real ring level == VR_TARGET, not just g_fill_ema) is a no-op.
 * -------------------------------------------------------------------------- */
static void test_trim_direction(void)
{
    video_audio_start();
    g_fill_ema = VR_TARGET + 800;                 /* ring reading full */
    trim_step();
    CHECK(g_step > g_step_base, "trim_step speeds up (bigger step) when fill is above target");

    video_audio_start();
    g_fill_ema = VR_TARGET - 800;                 /* ring reading empty */
    trim_step();
    CHECK(g_step < g_step_base, "trim_step slows down (smaller step) when fill is below target");

    video_audio_start();
    for (int i = 0; i < VR_TARGET; i++) ring_push(0);   /* actually AT target, not just g_fill_ema */
    trim_step();
    CHECK(g_step == g_step_base, "trim_step is a no-op when the ring is exactly at target");
    OK("trim_step direction matches its comment (fuller -> faster -> drains)");
}

/* --------------------------------------------------------------------------
 * trim_step() clamps to +/-1% full-scale deflection (TRIM_MAX_PCT_X100=100),
 * however far the ring is from target.
 * -------------------------------------------------------------------------- */
static void test_trim_clamp(void)
{
    video_audio_start();
    g_fill_ema = VR_TARGET + 1000000;             /* absurd error: must still clamp */
    trim_step();
    uint32_t max_step = g_step_base + (uint32_t)(((int64_t)g_step_base * TRIM_MAX_PCT_X100) / 10000);
    CHECK(g_step == max_step, "trim_step clamps at +1% full scale");

    video_audio_start();
    g_fill_ema = VR_TARGET - 1000000;
    trim_step();
    uint32_t min_step = g_step_base - (uint32_t)(((int64_t)g_step_base * TRIM_MAX_PCT_X100) / 10000);
    CHECK(g_step == min_step, "trim_step clamps at -1% full scale");
    OK("trim_step clamps to +/-1% regardless of how far off target");
}

/* --------------------------------------------------------------------------
 * video_audio_stop() (a seek goes through this) must reset the servo, not
 * just the ring — CLAUDE.md: "or the empty ring reads as starving and the
 * trim slams."
 * -------------------------------------------------------------------------- */
static void test_stop_resets_servo(void)
{
    video_audio_start();
    g_fill_ema = VR_TARGET + 900;                 /* simulate drift before a seek */
    g_step = g_step_base + 12345;
    for (int i = 0; i < 100; i++) ring_push(0);   /* pretend the ring has queued audio */
    video_audio_stop();
    CHECK(g_fill_ema == VR_TARGET, "stop: g_fill_ema reset to VR_TARGET");
    CHECK(g_step == g_step_base, "stop: g_step reset to untrimmed");
    CHECK(video_audio_ring_count() == 0, "stop: ring drained to empty");
    OK("video_audio_stop resets the servo, not just the ring");
}

/* --------------------------------------------------------------------------
 * The regression this file exists for. Feed REAL decoded audio, one MP3
 * frame at a time, while draining it slightly slower than it's produced (a
 * fixed-rate mismatch well inside the servo's +/-1% authority — the drifting-
 * clock scenario CLAUDE.md describes). With the PI servo doing its job the
 * ring CONVERGES ON VR_TARGET — not merely to some plateau above the prefetch
 * gate. A pure-proportional servo needs a standing fill error to command the
 * standing correction a sustained mismatch requires, so it settles at an OFFSET
 * above target (which can park the ring near the gate that latches playback);
 * the integral term drives that steady-state error to zero. The "calibration"
 * pass first measures this fixture's true untrimmed production rate (resetting the
 * servo state after each frame so its own reaction doesn't contaminate the
 * measurement) instead of hardcoding a rate that only holds for one exact
 * ffmpeg/lame version's frame sizes.
 *
 * Revert trim_step() to a no-op (g_step = g_step_base always) and this test
 * goes RED: ring_free() never plateaus, it keeps falling — the "prefetch gate
 * latches shut" failure mode. See this file's tests/run.sh entry for the
 * recorded RED/GREEN proof.
 * -------------------------------------------------------------------------- */
#define MP3_PATH "/tmp/mtest/video_audio_test.mp3"
#define MAX_FRAMES 5000

static uint8_t *load_mp3(long *out_len)
{
    FILE *f = fopen(MP3_PATH, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_len = n;
    return buf;
}

/* Find every real MP3 frame's (offset, length) in one pass with a throwaway
 * decoder, so the servo test can feed exactly one frame per call — see the
 * file header comment for why arbitrary byte windows don't decode reliably. */
static int build_frame_table(const uint8_t *mp3, long len, long *offs, int *lens, int max)
{
    mp3dec_t scratch;
    mp3dec_init(&scratch);
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    long pos = 0;
    int nf = 0;
    while (pos < len && nf < max) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&scratch, mp3 + pos, (int)(len - pos), pcm, &info);
        if (info.frame_bytes == 0) break;             /* no more data / no sync */
        if (samples > 0) { offs[nf] = pos; lens[nf] = info.frame_bytes; nf++; }
        pos += info.frame_bytes;
    }
    return nf;
}

static void test_servo_holds_under_sustained_mismatch(const uint8_t *mp3, long *offs, int *lens, int nf)
{
    /* Calibration: feed each real frame, draining it fully every time so the
     * ring never approaches target (never triggers a trim reaction), AND
     * force the servo state back to neutral after each call so any reaction
     * that DID slip through can't bias the measurement. What's left is the
     * fixture's true untrimmed production rate. */
    video_audio_start();
    long produced = 0;
    for (int i = 0; i < nf; i++) {
        video_audio_feed(mp3 + offs[i], lens[i]);
        produced += video_audio_ring_count();
        g_head = g_tail;                               /* drain fully */
        g_step = g_step_base; g_fill_ema = VR_TARGET;   /* undo any servo reaction */
    }
    CHECK(produced > 0, "calibration: the fixture actually decodes to samples");
    double nominal = (double)produced / nf;

    /* Drift pass: drain ~0.3% less than nominal every frame (CLAUDE.md's own
     * example mismatch) — comfortably inside the servo's +/-1% authority, so
     * a real equilibrium exists for it to find. */
    video_audio_start();
    int drain = (int)(nominal * 0.997);
    CHECK(drain > 0 && drain < (int)nominal, "drift pass: drain rate is a plausible sustained mismatch");

    int q3_sum = 0, q3_n = 0, q4_sum = 0, q4_n = 0;   /* 3rd / 4th quarter of the run */
    for (int i = 0; i < nf; i++) {
        video_audio_feed(mp3 + offs[i], lens[i]);
        isr_drain(drain);
        if (i >= nf / 2 && i < 3 * nf / 4)  { q3_sum += video_audio_ring_free(); q3_n++; }
        if (i >= 3 * nf / 4)                { q4_sum += video_audio_ring_free(); q4_n++; }
    }
    int q3_avg = q3_n ? q3_sum / q3_n : 0;
    int q4_avg = q4_n ? q4_sum / q4_n : 0;
    printf("  (ring_free avg: 3rd quarter=%d, 4th quarter=%d, PF_AUDIO_HEADROOM=2400)\n", q3_avg, q4_avg);

    CHECK(q4_avg > 2400, "servo holds ring_free() above the prefetch gate under sustained under-drain");
    /* Converged, not just still-above-threshold-by-luck: the 3rd and 4th
     * quarter averages should be close to each other, not still trending
     * down (which is what "reverted trim_step" looks like — see RED proof). */
    int drift = q3_avg - q4_avg;
    if (drift < 0) drift = -drift;
    CHECK(drift < 150, "ring_free() has settled by the back half, not still declining");

    /* The PI invariant (this is the strengthening over the old proportional-only
     * assertion, which accepted any plateau below the gate): the ring converges
     * ON VR_TARGET, not to an offset above it. ring_count = (VR_SIZE-1) - free,
     * so the back-half ring level is (VR_SIZE-1) - q4_avg. A pure-proportional
     * servo settles ~100 samples above target at this ~0.3% mismatch (it needs
     * the standing error to hold the standing trim); the integral term erases
     * that. Require the converged level within 50 of VR_TARGET — met by PI
     * (offset ~5) and FAILED by a proportional-only servo (offset ~109, proven
     * by rebuilding this file with -DTRIM_KI_DIV=1000000000 to null the
     * integral: the RED the strengthening is written against). */
    int q4_ring = (VR_SIZE - 1) - q4_avg;
    int off = q4_ring - VR_TARGET;
    if (off < 0) off = -off;
    printf("  (converged ring level=%d, VR_TARGET=%d, offset=%d)\n", q4_ring, VR_TARGET, off);
    CHECK(off < 50, "PI servo converges the ring ON VR_TARGET (no proportional plateau offset)");
    OK("trim_step converges the ring to VR_TARGET under a sustained drain mismatch");
}

int main(void)
{
    printf("== test_video_audio ==\n");
    test_start_state();
    test_target_below_gate();
    test_trim_direction();
    test_trim_clamp();
    test_stop_resets_servo();

    long mp3_len = 0;
    uint8_t *mp3 = load_mp3(&mp3_len);
    if (!mp3) {
        printf("SKIP servo drift test: %s not present "
               "(tests/run.sh generates it with ffmpeg; ffmpeg missing here)\n", MP3_PATH);
    } else {
        static long offs[MAX_FRAMES];
        static int  lens[MAX_FRAMES];
        int nf = build_frame_table(mp3, mp3_len, offs, lens, MAX_FRAMES);
        if (nf < 500) {
            printf("SKIP servo drift test: only %d MP3 frames decoded from the fixture "
                   "(need >=500 for the servo to reach a stable plateau)\n", nf);
        } else {
            test_servo_holds_under_sustained_mismatch(mp3, offs, lens, nf);
        }
        free(mp3);
    }

    if (g_failures) { printf("FAILED (%d)\n", g_failures); return 1; }
    printf("all video_audio tests passed\n");
    return 0;
}
