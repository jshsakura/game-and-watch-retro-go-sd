// Chunk-fed MP3 audio — see video_audio.h. Mirrors music_audio.c's proven
// decode -> downmix -> resample -> ring path, but the input is pushed in (the
// AVI's audio chunks) instead of pulled from a file.

#include "video_audio.h"
#include "gw_audio.h"          // music_attach() + AUDIO_SAMPLE_RATE
#include "minimp3.h"
#include <string.h>

#define VR_SIZE   4096         // power of two — ~85ms of 48kHz buffer
#define VR_MASK   (VR_SIZE - 1)
#define VIN_MAX   2048         // MP3 input accumulation (frames are <600 bytes)

// --- clock trim -------------------------------------------------------------
// Nothing synchronises the two clocks in this player. The SAI ISR drains this
// ring at the audio PLL's REAL rate; the demuxer fills it one AVI audio chunk
// per displayed video frame, i.e. at the rate video_play.c paces frames by
// SysTick. Those are different oscillators and different dividers, so they
// differ — and the error only ever accumulates in one direction.
//
// The ring is 85ms. A 0.3% mismatch fills it in under a minute; a 0.05% one
// takes several. Once it is full the prefetcher's PF_AUDIO_HEADROOM gate can
// never open again, so every frame read becomes a blocking one and playback
// degrades from smooth to stuttering and stays there — "fine for four minutes,
// then progressively worse, and worse still on a long clip".
//
// So close the loop: hold the ring near VR_TARGET by trimming the resample step
// a fraction of a percent. Consuming input slightly faster (a bigger step) emits
// fewer samples per MP3 frame and drains a filling ring, and vice versa. Full
// deflection is 1%, which is 17 cents of pitch — inaudible, and far more than
// any real crystal error needs.
//
// VR_TARGET must stay under the level at which the prefetch gate closes
// (VR_SIZE-1 - PF_AUDIO_HEADROOM = 1695 samples), or holding the target would
// itself be what keeps the prefetcher off.
#define VR_TARGET  1200        // ~25ms held in the ring
#define TRIM_SPAN  1024        // fill error at which the PROPORTIONAL term reaches full scale
#define TRIM_MAX_PCT_X100  100 // 1.00% maximum step deflection (servo authority)

// The trim is a PI servo. A pure proportional term needs a standing fill error
// to command a standing correction, so under a sustained clock mismatch it
// settles at a PLATEAU offset above VR_TARGET (e.g. a 1% mismatch demands the
// full 1% deflection, which a P-only servo can only reach with err ~= TRIM_SPAN
// = the ring parked at VR_TARGET+1024 = 2224, well above the 1695 gate — so the
// prefetcher stays latched off exactly when the servo is "working"). The slow
// integral term drives that steady-state error to zero: the ring converges ON
// VR_TARGET for any mismatch inside authority, not to an offset near the gate.
// TRIM_KI_DIV is the integrator gain denominator (bigger = slower); tuned on the
// QEMU M7 rig so ppm=0 stays flat (no hunting) and 1% mismatch converges to
// VR_TARGET rather than plateauing high.
#ifndef TRIM_KI_DIV
#define TRIM_KI_DIV  256       // integral gain denominator (err-per-call -> 0.01%)
#endif

static int16_t           g_ring[VR_SIZE];
static volatile uint16_t g_head, g_tail;

static mp3dec_t  g_mp3;
static int16_t   g_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
/* mp3dec_decode_frame returns PER-CHANNEL sample counts (<=1152), so the mono
 * downmix needs only half of MAX_SAMPLES_PER_FRAME (which counts both
 * channels interleaved). The overlay BSS sits within bytes of its limit. */
static int16_t   g_mono[MINIMP3_MAX_SAMPLES_PER_FRAME / 2];
static int       g_frame_n;            // mono samples pending in g_mono
static uint32_t  g_phase, g_step;      // 16.16 resample index / step (trimmed)
static uint32_t  g_step_base;          // ...and its untrimmed source-rate value
static int       g_fill_ema;           // low-passed ring level the trim servos on
static int32_t   g_fill_integ;         // integral accumulator (err summed over calls)
static int16_t   g_prev;               // last sample of the PREVIOUS frame

static uint8_t   g_in[VIN_MAX];        // leftover undecoded MP3 bytes
static int       g_in_len;

static int ring_count(void) { return (g_head - g_tail) & VR_MASK; }

// Re-aim the resample step at VR_TARGET. Called once per fed chunk, on a level
// that is low-passed first: the ring swings by a whole chunk within one video
// frame, and servoing on that instantaneous value would just modulate the pitch
// at the frame rate instead of correcting the drift underneath it.
static void trim_step(void)
{
    if (g_step_base == 0) return;

    g_fill_ema += (ring_count() - g_fill_ema) / 8;         // EMA, ~8-chunk window

    int err = g_fill_ema - VR_TARGET;                       // >0: too full -> consume faster
    if (err >  TRIM_SPAN) err =  TRIM_SPAN;
    if (err < -TRIM_SPAN) err = -TRIM_SPAN;

    // Proportional and integral terms, both in units of 0.01% of the step.
    int32_t p = (int32_t)err * TRIM_MAX_PCT_X100 / TRIM_SPAN;   // +/-100 for err = +/-TRIM_SPAN
    g_fill_integ += err;                                        // slow error accumulation
    int32_t pct = p + g_fill_integ / TRIM_KI_DIV;

    // Anti-windup: the TOTAL deflection is the servo's only authority, so clamp
    // it to +/-TRIM_MAX_PCT_X100 and, when it saturates, hold the integrator at
    // exactly the value that keeps the total on the edge — it never winds up past
    // what the step can express (so it unwinds the instant the error reverses).
    if (pct > TRIM_MAX_PCT_X100) {
        pct = TRIM_MAX_PCT_X100;
        g_fill_integ = (int32_t)(TRIM_MAX_PCT_X100 - p) * TRIM_KI_DIV;
    } else if (pct < -TRIM_MAX_PCT_X100) {
        pct = -TRIM_MAX_PCT_X100;
        g_fill_integ = (int32_t)(-TRIM_MAX_PCT_X100 - p) * TRIM_KI_DIV;
    }

    int32_t adj = (int32_t)(((int64_t)g_step_base * pct) / 10000);
    g_step = (uint32_t)((int32_t)g_step_base + adj);
}

// --- non-latching saturation valve ------------------------------------------
// The PI servo cancels any mismatch INSIDE its +/-1% authority, but a real crystal
// pair (or a badly muxed clip) can drift faster than that. Then production beats
// consumption for good and the ring would fill to VR_SIZE and PIN there — its
// per-frame trough parked above the prefetch gate (VR_SIZE-1-PF_AUDIO_HEADROOM =
// 1695), which is the latch. VR_SIZE (4095) is just an accidental, far-too-high
// ceiling; replace it with a deliberate one.
//
// VR_VALVE_CAP is the highest count we let the ring hold. Its job is that a
// valve-PINNED ring (drift beyond the servo's authority) still drains below the
// prefetch gate (1695) within one frame — pinned trough ~= cap - one drained
// chunk (~895 on the rig) — so the gate always reopens and prefetch keeps hiding
// the bursty MJPEG reads (rd stays ~0). That needs cap < ~2590. The prefetcher's
// own healthy peak is ~2620 (it fills to the 1695 gate then pushes one whole
// ~1152-sample audio chunk past it before the next gate check), so a cap in this
// range clips a one-off ~13ms of audio during the startup pre-roll ramp; steady
// state peaks ~2480 and the valve never fires again. That one-time pre-roll blip
// is the price of keeping playback smooth at the very edge of authority, where a
// higher cap would park the ring above the gate and let burst frames blow the
// per-frame budget. Inside authority the PI holds the ring at VR_TARGET and the
// valve is dormant.
#ifndef VR_VALVE_CAP
#define VR_VALVE_CAP 2560
#endif

uint32_t g_video_audio_drops;          // samples the valve has dropped (diagnostic)

static int ring_push(int16_t s)
{
    uint16_t n = (g_head + 1) & VR_MASK;
    if (n == g_tail) return 0;          // full (unreachable while the valve holds)
    g_ring[g_head] = s;
    g_head = n;
    // Valve: if the ring has climbed past the cap, drop the OLDEST sample so the
    // count can never sit high enough to hold the prefetch gate shut. Dropping
    // the stale end (advance tail) keeps A/V sync tighter than refusing the fresh
    // one. The SAI ISR also advances g_tail, but the store is a single aligned
    // 16-bit write and the valve fires only under out-of-authority drift, so a
    // lost-update race is at worst one stale sample — never a corrupt index.
    if (ring_count() > VR_VALVE_CAP) {
        g_tail = (g_tail + 1) & VR_MASK;
        g_video_audio_drops++;
    }
    return 1;
}

void video_audio_start(void)
{
    mp3dec_init(&g_mp3);
    g_head = g_tail = 0;
    g_frame_n = 0;
    g_phase = 0;
    g_prev = 0;                                            // no left-hand sample yet
    g_step_base = ((uint32_t)44100 << 16) / AUDIO_SAMPLE_RATE;  // until the first frame
    g_step = g_step_base;
    g_fill_ema = VR_TARGET;                                // start centred: no kick at t=0
    g_fill_integ = 0;                                      // integrator starts unwound
    g_in_len = 0;
    music_attach(g_ring, VR_SIZE, &g_head, &g_tail);        // ISR reads this ring
}

int video_audio_ring_count(void) { return ring_count(); }
int video_audio_ring_free(void)  { return VR_SIZE - 1 - ring_count(); }

void video_audio_stop(void)
{
    g_head = g_tail = 0;                 // drain -> silence (ISR reads an empty ring)
    g_frame_n = 0;
    g_prev = 0;
    g_in_len = 0;
    g_fill_ema = VR_TARGET;              // a seek empties the ring; don't let the
    g_fill_integ = 0;                    // servo read that as "starving" and slam
    g_step = g_step_base;

}

// Resample the pending mono frame to 48 kHz and push it to the ring. Returns 0
// if the ring filled mid-frame (the rest stays for the next call).
//
// Linear interpolation, not nearest-sample: with g_step != 65536 (any source
// that isn't 48 kHz — the encoder emits 44.1 kHz) picking the nearest sample
// folds an image of the source rate into the audible band. Interpolating from
// the PREVIOUS sample (g_prev covers index -1) avoids needing the next frame,
// at the cost of a constant one-sample delay. See music_audio.c for the numbers.
static int drain_pending(void)
{
    while ((g_phase >> 16) < (uint32_t)g_frame_n) {
        const uint32_t i = g_phase >> 16;
        const int32_t  a = (i == 0) ? g_prev : g_mono[i - 1];
        const int32_t  b = g_mono[i];
        // (b - a) spans 17 bits, the fraction 16 -> the product needs 64 bits
        const int16_t  s = (int16_t)(a + (int32_t)(((int64_t)(b - a) * (g_phase & 0xFFFF)) >> 16));
        if (!ring_push(s)) return 0;        // ring full: resume here next call
        g_phase += g_step;
    }
    if (g_frame_n > 0) g_prev = g_mono[g_frame_n - 1];   // only once the frame is spent
    g_phase -= (uint32_t)g_frame_n << 16;   // carry the fractional remainder
    g_frame_n = 0;
    return 1;
}

void video_audio_feed(const uint8_t *mp3, int len)
{
    if (len <= 0) return;
    if (len > VIN_MAX) { mp3 += len - VIN_MAX; len = VIN_MAX; }   // pathological clamp

    // Append to the accumulation buffer, dropping the oldest if it would overflow.
    if (g_in_len + len > VIN_MAX) {
        int drop = g_in_len + len - VIN_MAX;
        memmove(g_in, g_in + drop, g_in_len - drop);
        g_in_len -= drop;
    }
    memcpy(g_in + g_in_len, mp3, len);
    g_in_len += len;

    trim_step();   // re-aim the resampler at VR_TARGET before emitting anything

    // Finish a frame left half-drained by a previously-full ring.
    if (g_frame_n > 0 && !drain_pending()) return;

    int pos = 0;
    while (pos < g_in_len) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&g_mp3, g_in + pos, g_in_len - pos, g_pcm, &info);
        pos += info.frame_bytes;
        if (samples > 0) {
            if (info.channels >= 2)
                for (int i = 0; i < samples; i++)
                    g_mono[i] = (int16_t)(((int)g_pcm[2 * i] + g_pcm[2 * i + 1]) / 2);
            else
                for (int i = 0; i < samples; i++)
                    g_mono[i] = g_pcm[i];
            g_frame_n = samples;
            if (info.hz > 0) {
                uint32_t base = ((uint32_t)info.hz << 16) / AUDIO_SAMPLE_RATE;
                if (base != g_step_base) { g_step_base = base; trim_step(); }
            }
            if (!drain_pending()) break;     // ring full -> stop; keep remaining input
        } else if (info.frame_bytes == 0) {
            break;                           // need more data
        }
    }
    // Drop the bytes we consumed.
    if (pos > 0) {
        if (pos > g_in_len) pos = g_in_len;
        memmove(g_in, g_in + pos, g_in_len - pos);
        g_in_len -= pos;
    }
}
