// Streaming MP3 audio engine — see media_audio.h.

#include "media_audio.h"
#include "minimp3.h"
#include "gw_audio.h"          // AUDIO_SAMPLE_RATE
#include <stdio.h>
#include <string.h>

#define MP3_IN_BUF  (16 * 1024)

static mp3dec_t  g_mp3;
static FILE     *g_fp;
static uint8_t   g_in[MP3_IN_BUF];
static int       g_in_len, g_in_pos;
static bool      g_file_eof;
static int16_t   g_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int16_t   g_mono[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int       g_frame_n;     // mono samples currently in g_mono
static uint32_t  g_phase;       // 16.16 read index within the current frame
static uint32_t  g_step;        // (in_rate << 16) / 48000
static bool      g_eof;         // decode reached end of stream
static int       g_bitrate;     // kbps of the last decoded frame
static int       g_hz;          // source sample rate of the last frame
static int       g_chan;        // channels of the last frame

static long      g_data_off;    // first audio byte (past ID3v2 tag)
static long      g_audio_size;  // bytes of audio data (file size - g_data_off)

// --- decoded-PCM ring (48 kHz mono) -----------------------------------------
#define RING_SIZE  8192            // power of two
#define RING_MASK  (RING_SIZE - 1)
static int16_t g_ring[RING_SIZE];
static int     g_head, g_tail, g_count;

static void ring_push(int16_t s)
{
    if (g_count >= RING_SIZE) return;
    g_ring[g_head] = s;
    g_head = (g_head + 1) & RING_MASK;
    g_count++;
}

void audio_ring_reset(void) { g_head = g_tail = g_count = 0; }

int16_t audio_pull(void)
{
    if (g_count == 0) return 0;
    int16_t s = g_ring[g_tail];
    g_tail = (g_tail + 1) & RING_MASK;
    g_count--;
    return s;
}

int  audio_ring_count(void) { return g_count; }
bool audio_eof(void)        { return g_eof; }
int  audio_bitrate_kbps(void) { return g_bitrate; }
int  audio_src_hz(void)     { return g_hz; }
int  audio_channels(void)   { return g_chan; }

// --- decode -----------------------------------------------------------------

static void refill(void)
{
    if (g_in_pos > 0) {
        int remain = g_in_len - g_in_pos;
        if (remain > 0)
            memmove(g_in, g_in + g_in_pos, remain);
        g_in_len = remain;
        g_in_pos = 0;
    }
    if (!g_file_eof) {
        int space = MP3_IN_BUF - g_in_len;
        int got = space > 0 ? (int)fread(g_in + g_in_len, 1, space, g_fp) : 0;
        if (got <= 0) g_file_eof = true;
        else          g_in_len += got;
    }
}

// Decode one MP3 frame into g_mono; returns false at end of stream.
static bool decode_frame(void)
{
    for (;;) {
        if ((g_in_len - g_in_pos) < 2048 && !g_file_eof)
            refill();

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&g_mp3, g_in + g_in_pos,
                                          g_in_len - g_in_pos, g_pcm, &info);
        g_in_pos += info.frame_bytes;

        if (samples > 0) {
            if (info.channels >= 2)
                for (int i = 0; i < samples; i++)
                    g_mono[i] = (int16_t)(((int)g_pcm[2 * i] + g_pcm[2 * i + 1]) / 2);
            else
                for (int i = 0; i < samples; i++)
                    g_mono[i] = g_pcm[i];
            g_frame_n = samples;
            if (info.hz > 0) { g_hz = info.hz; g_step = ((uint32_t)info.hz << 16) / AUDIO_SAMPLE_RATE; }
            if (info.bitrate_kbps > 0) g_bitrate = info.bitrate_kbps;
            if (info.channels > 0) g_chan = info.channels;
            return true;
        }
        if (info.frame_bytes == 0) {
            if (g_file_eof) return false;
            refill();
            if ((g_in_len - g_in_pos) == 0) return false;
        }
    }
}

void audio_pump(int target)
{
    while (g_count < target && !g_eof) {
        while ((g_phase >> 16) >= (uint32_t)g_frame_n) {
            g_phase -= (uint32_t)g_frame_n << 16;
            if (!decode_frame()) { g_eof = true; break; }
        }
        if (g_eof) break;
        ring_push(g_mono[g_phase >> 16]);
        g_phase += g_step;
    }
}

// --- open / close / seek ----------------------------------------------------

static void reset_decoder(void)
{
    mp3dec_init(&g_mp3);
    g_in_len = g_in_pos = 0;
    g_file_eof = (g_fp == NULL);
    g_frame_n = 0;
    g_phase = 0;
    g_eof = false;
    audio_ring_reset();
}

bool audio_open(const char *path)
{
    audio_close();
    g_fp = fopen(path, "rb");
    g_step = ((uint32_t)44100 << 16) / AUDIO_SAMPLE_RATE;
    g_bitrate = 0; g_hz = 0; g_chan = 0;
    g_data_off = 0; g_audio_size = 0;

    if (g_fp) {
        uint8_t h[10];
        if (fread(h, 1, 10, g_fp) == 10 && memcmp(h, "ID3", 3) == 0) {
            g_data_off = 10 + (long)(((uint32_t)(h[6] & 0x7f) << 21) |
                ((uint32_t)(h[7] & 0x7f) << 14) | ((uint32_t)(h[8] & 0x7f) << 7) |
                (uint32_t)(h[9] & 0x7f));
        }
        fseek(g_fp, 0, SEEK_END);
        long sz = ftell(g_fp);
        g_audio_size = sz - g_data_off;
        fseek(g_fp, g_data_off, SEEK_SET);
    }
    reset_decoder();
    return g_fp != NULL;
}

void audio_close(void)
{
    if (g_fp) { fclose(g_fp); g_fp = NULL; }
}

void audio_seek(float frac)
{
    if (!g_fp || g_audio_size <= 0) return;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 0.999f) frac = 0.999f;
    long off = g_data_off + (long)(frac * (float)g_audio_size);
    fseek(g_fp, off, SEEK_SET);
    reset_decoder();
    audio_pump(AUDIO_PUMP_TARGET);
}

int audio_duration_sec(void)
{
    if (g_bitrate <= 0 || g_audio_size <= 0) return 0;
    return (int)((long long)g_audio_size * 8 / ((long long)g_bitrate * 1000));
}

// --- standalone quick duration (list rows) ----------------------------------

int audio_quick_duration(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t h[10];
    long off = 0;
    if (fread(h, 1, 10, f) == 10 && memcmp(h, "ID3", 3) == 0)
        off = 10 + (long)(((uint32_t)(h[6] & 0x7f) << 21) | ((uint32_t)(h[7] & 0x7f) << 14) |
                          ((uint32_t)(h[8] & 0x7f) << 7) | (uint32_t)(h[9] & 0x7f));
    fseek(f, off, SEEK_SET);
    int got = (int)fread(g_in, 1, 4096, f);     // reuse input buffer (not playing)
    fclose(f);
    if (got <= 0) return 0;

    mp3dec_init(&g_mp3);
    mp3dec_frame_info_t info;
    int pos = 0, br = 0;
    while (pos < got) {
        int s = mp3dec_decode_frame(&g_mp3, g_in + pos, got - pos, g_pcm, &info);
        pos += info.frame_bytes;
        if (info.frame_bytes == 0) break;
        if (s > 0 && info.bitrate_kbps > 0) { br = info.bitrate_kbps; break; }
    }
    if (br <= 0) return 0;
    return (int)((long long)(sz - off) * 8 / ((long long)br * 1000));
}
