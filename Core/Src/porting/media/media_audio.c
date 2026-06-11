// Streaming MP3 + WAV(PCM) audio engine — see media_audio.h. Both formats feed
// the same mono / 48 kHz resample ring; WAV (8/16/24/32-bit, any rate) reuses
// the MP3 input buffer so it costs no extra RAM.

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

static long      g_data_off;    // first audio byte (past ID3v2 tag / WAV header)
static long      g_audio_size;  // bytes of audio data (file size - g_data_off)

// WAV (uncompressed PCM) support — reuses the same ring/resample path as MP3.
static bool      g_is_wav;
static int       g_wav_hz, g_wav_chan, g_wav_bits;
static long      g_wav_pos;     // bytes already read out of the data chunk

// Parse a RIFF/WAVE header from `f`: fill rate/channels/bits and the data chunk
// offset+size. Returns false if not a PCM WAV we can play.
static bool wav_parse_fp(FILE *f, int *hz, int *chan, int *bits, long *doff, long *dsz)
{
    uint8_t hdr[12];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(hdr, 1, 12, f) != 12) return false;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return false;
    bool have_fmt = false, have_data = false;
    long pos = 12;
    for (int guard = 0; guard < 64 && !(have_fmt && have_data); guard++) {
        uint8_t ch[8];
        if (fseek(f, pos, SEEK_SET) != 0 || fread(ch, 1, 8, f) != 8) break;
        uint32_t csz = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fm[16];
            if (fread(fm, 1, 16, f) == 16) {
                int fmt = fm[0] | (fm[1] << 8);
                *chan = fm[2] | (fm[3] << 8);
                *hz   = fm[4] | (fm[5] << 8) | (fm[6] << 16) | ((uint32_t)fm[7] << 24);
                *bits = fm[14] | (fm[15] << 8);
                if (fmt == 1 || fmt == 0xFFFE) have_fmt = true;   // PCM / extensible
            }
        } else if (memcmp(ch, "data", 4) == 0) {
            *doff = pos + 8; *dsz = (long)csz; have_data = true;
        }
        pos += 8 + (long)csz + (csz & 1);                          // word-aligned chunks
    }
    if (!have_fmt || !have_data || *chan < 1) return false;
    if (*bits != 8 && *bits != 16 && *bits != 24 && *bits != 32) return false;
    return true;
}

// --- decoded-PCM ring (48 kHz mono) -----------------------------------------
#define RING_SIZE  8192            // power of two
#define RING_MASK  (RING_SIZE - 1)
static int16_t g_ring[RING_SIZE];
// Lock-free single-producer (decode) / single-consumer (DMA ISR) ring: the
// producer only writes g_head, the consumer only writes g_tail, so it is
// ISR-safe without a shared count.
static volatile uint16_t g_head, g_tail;

static void ring_push(int16_t s)
{
    uint16_t next = (g_head + 1) & RING_MASK;
    if (next == g_tail) return;            // full
    g_ring[g_head] = s;
    g_head = next;
}

void audio_ring_reset(void) { g_head = g_tail = 0; }

int16_t audio_pull(void)
{
    if (g_tail == g_head) return 0;        // empty
    int16_t s = g_ring[g_tail];
    g_tail = (g_tail + 1) & RING_MASK;
    return s;
}

int  audio_ring_count(void) { return (g_head - g_tail) & RING_MASK; }

// --- ISR-driven playback ----------------------------------------------------
// The SAI DMA ISR fills the next half-buffer straight from the ring, decoupled
// from rendering, so a slow frame can't starve the ~22ms DMA buffer (the way
// old MP3 players did it). The main loop only keeps the ring full (audio_pump).
extern void ui_vis_push(int16_t);
static volatile int16_t  g_isr_vol = 0;     // 0..256
static volatile uint8_t  g_isr_silent = 1;  // output silence (paused/stopped/seeking)
static volatile uint32_t g_isr_played;      // samples played so far (position)

void audio_isr_set(int vol, bool silent) { g_isr_vol = (int16_t)vol; g_isr_silent = silent ? 1 : 0; }
void audio_isr_setpos(uint32_t samples)  { g_isr_played = samples; }
uint32_t audio_isr_pos(void)             { return g_isr_played; }

// Called from HAL_SAI_Tx(Half)CpltCallback — fills the just-freed half-buffer.
void audio_isr_fill(void)
{
    int16_t *buf = audio_get_active_buffer();
    int len = audio_get_buffer_length();
    int vol = g_isr_vol;
    bool silent = g_isr_silent;
    for (int i = 0; i < len; i++) {
        int16_t sm = silent ? 0 : audio_pull();
        buf[i] = (int16_t)((sm * vol) >> 8);
        if (!(i & 1)) ui_vis_push(sm);
    }
    if (!silent) g_isr_played += len;
}
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

// Read one block of WAV PCM into g_mono (downmixed to mono int16). Reuses g_in
// as the byte buffer, so no extra RAM.
static bool wav_decode_frame(void)
{
    int fb = g_wav_chan * (g_wav_bits / 8), bytes = g_wav_bits / 8;
    long remain = g_audio_size - g_wav_pos;
    if (fb <= 0 || remain < fb) return false;
    int frames = 1152;
    if ((long)frames * fb > remain)      frames = (int)(remain / fb);
    if ((long)frames * fb > MP3_IN_BUF)  frames = MP3_IN_BUF / fb;
    int got = (int)fread(g_in, 1, (size_t)frames * fb, g_fp);
    g_wav_pos += got;
    int gf = got / fb;
    for (int i = 0; i < gf; i++) {
        int32_t acc = 0;
        const uint8_t *p = g_in + (long)i * fb;
        for (int c = 0; c < g_wav_chan; c++, p += bytes) {
            int32_t s;
            if (g_wav_bits == 8)       s = ((int)p[0] - 128) << 8;        // unsigned 8-bit
            else if (g_wav_bits == 16) s = (int16_t)(p[0] | (p[1] << 8));
            else if (g_wav_bits == 24) s = (int16_t)(p[1] | (p[2] << 8)); // high 16 of 24
            else                       s = (int16_t)(p[2] | (p[3] << 8)); // high 16 of 32
            acc += s;
        }
        g_mono[i] = (int16_t)(acc / g_wav_chan);
    }
    g_frame_n = gf;
    return gf > 0;
}

// Decode one frame into g_mono; returns false at end of stream.
static bool decode_frame(void)
{
    if (g_is_wav) return wav_decode_frame();
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
    while (audio_ring_count() < target && !g_eof) {
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
    g_is_wav = false; g_wav_pos = 0;

    if (g_fp) {
        if (wav_parse_fp(g_fp, &g_wav_hz, &g_wav_chan, &g_wav_bits, &g_data_off, &g_audio_size)) {
            g_is_wav = true;
            g_hz = g_wav_hz; g_chan = g_wav_chan;
            g_step = ((uint32_t)g_wav_hz << 16) / AUDIO_SAMPLE_RATE;
            g_bitrate = (int)((long long)g_wav_hz * g_wav_chan * g_wav_bits / 1000);
        } else {
            uint8_t h[10];
            fseek(g_fp, 0, SEEK_SET);
            if (fread(h, 1, 10, g_fp) == 10 && memcmp(h, "ID3", 3) == 0) {
                g_data_off = 10 + (long)(((uint32_t)(h[6] & 0x7f) << 21) |
                    ((uint32_t)(h[7] & 0x7f) << 14) | ((uint32_t)(h[8] & 0x7f) << 7) |
                    (uint32_t)(h[9] & 0x7f));
            }
            fseek(g_fp, 0, SEEK_END);
            long sz = ftell(g_fp);
            g_audio_size = sz - g_data_off;
        }
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
    if (g_is_wav) {
        int fb = g_wav_chan * (g_wav_bits / 8);
        if (fb > 0) off -= (off - g_data_off) % fb;   // align to a sample frame
        g_wav_pos = off - g_data_off;
    }
    fseek(g_fp, off, SEEK_SET);
    reset_decoder();
    audio_pump(AUDIO_PUMP_TARGET);
}

int audio_duration_sec(void)
{
    if (g_is_wav) {
        int fb = g_wav_chan * (g_wav_bits / 8);
        if (fb <= 0 || g_wav_hz <= 0) return 0;
        return (int)(g_audio_size / fb / g_wav_hz);
    }
    if (g_bitrate <= 0 || g_audio_size <= 0) return 0;
    return (int)((long long)g_audio_size * 8 / ((long long)g_bitrate * 1000));
}

// --- standalone quick duration (list rows) ----------------------------------

int audio_quick_duration(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    int whz, wch, wb; long wdoff, wdsz;
    if (wav_parse_fp(f, &whz, &wch, &wb, &wdoff, &wdsz)) {   // WAV: from the header
        fclose(f);
        int fb = wch * (wb / 8);
        return (fb > 0 && whz > 0) ? (int)(wdsz / fb / whz) : 0;
    }

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
