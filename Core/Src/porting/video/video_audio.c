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

static int16_t           g_ring[VR_SIZE];
static volatile uint16_t g_head, g_tail;

static mp3dec_t  g_mp3;
static int16_t   g_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int16_t   g_mono[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int       g_frame_n;            // mono samples pending in g_mono
static uint32_t  g_phase, g_step;      // 16.16 resample index / step

static uint8_t   g_in[VIN_MAX];        // leftover undecoded MP3 bytes
static int       g_in_len;

static int ring_count(void) { return (g_head - g_tail) & VR_MASK; }

static int ring_push(int16_t s)
{
    uint16_t n = (g_head + 1) & VR_MASK;
    if (n == g_tail) return 0;          // full
    g_ring[g_head] = s;
    g_head = n;
    return 1;
}

void video_audio_start(void)
{
    mp3dec_init(&g_mp3);
    g_head = g_tail = 0;
    g_frame_n = 0;
    g_phase = 0;
    g_step = ((uint32_t)44100 << 16) / AUDIO_SAMPLE_RATE;   // until the first frame
    g_in_len = 0;
    music_attach(g_ring, VR_SIZE, &g_head, &g_tail);        // ISR reads this ring
}

int video_audio_ring_count(void) { return ring_count(); }

void video_audio_stop(void)
{
    g_head = g_tail = 0;                 // drain -> silence (ISR reads an empty ring)
    g_frame_n = 0;
    g_in_len = 0;
}

// Resample the pending mono frame to 48 kHz and push it to the ring. Returns 0
// if the ring filled mid-frame (the rest stays for the next call).
static int drain_pending(void)
{
    while ((g_phase >> 16) < (uint32_t)g_frame_n) {
        if (!ring_push(g_mono[g_phase >> 16])) return 0;    // ring full
        g_phase += g_step;
    }
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
            if (info.hz > 0) g_step = ((uint32_t)info.hz << 16) / AUDIO_SAMPLE_RATE;
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
