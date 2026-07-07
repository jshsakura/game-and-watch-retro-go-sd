/* PC Engine CD ADPCM (OKI MSM5205 4-bit voice/SFX). Ported from Mednafen
 * pce_fast (pcecd.c + okiadpcm.h). Registers $1808-$180E are routed here from
 * pce_scsi; the game DMAs CD data into the 64KB ADPCM RAM (pce_adpcm_dma_byte,
 * called from the SCSI->ADPCM drain) then triggers playback via $180D. We decode
 * at the programmed sample rate and resample to OUT_RATE for the PCE mixer.
 *
 * Simplifications vs Mednafen: the $180A write / read "pending" cycle timing is
 * collapsed to immediate (status bits 0x04/0x80 stay 0) — fine because the BIOS
 * load path uses the $180B DMA + $1803 DATA_DONE handshake, not those delays. */
#include "pce_adpcm.h"
#include <string.h>

#define ADPCM_RAM_SIZE 0x10000
#define OUT_RATE       44100   /* = PCE_SAMPLE_RATE: mixer runs at 44.1k now */

static const int StepSizes[49] = {
    16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
    157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,
    963,1060,1166,1282,1411,1552
};
static const int StepIdxDelta[16] = { -1,-1,-1,-1,2,4,6,8, -1,-1,-1,-1,2,4,6,8 };

static uint8_t  s_ram[ADPCM_RAM_SIZE];
static uint16_t s_addr, s_read_addr, s_write_addr, s_length;
static uint8_t  s_read_latch;   /* $180A read = one-read latency (see below) */
static uint8_t  s_last_cmd, s_freq;
static bool     s_playing, s_end, s_half, s_play_nibble;
static int32_t  s_cur;          /* decoder predictor, 12-bit (center 0x800) */
static int      s_ssi;          /* step-size index 0..48 */
static uint32_t s_phase;        /* fs->OUT_RATE resample accumulator */
static int16_t  s_held;         /* last decoded PCM sample (held between ticks) */

void pce_adpcm_reset(void)
{
    s_addr = s_read_addr = s_write_addr = s_length = 0;
    s_read_latch = 0;
    s_last_cmd = s_freq = 0;
    s_playing = s_end = s_half = s_play_nibble = false;
    s_cur = 0x800; s_ssi = 0; s_phase = 0; s_held = 0;
}

static int adpcm_decode(uint8_t nib)
{
    int d = StepSizes[s_ssi] * (2 * (nib & 7) + 1) / 8;
    if (nib & 8) d = -d;
    s_ssi += StepIdxDelta[nib];
    if (s_ssi < 0) s_ssi = 0; else if (s_ssi > 48) s_ssi = 48;
    s_cur = (s_cur + d) & 0xFFF;
    return s_cur;
}

/* Advance the decoder by one ADPCM nibble; updates s_held; handles end-of-sample. */
static void adpcm_tick(void)
{
    if (!s_playing) { s_held = 0; return; }
    uint8_t byte = s_ram[s_read_addr];
    uint8_t nib  = s_play_nibble ? (byte & 0x0F) : (byte >> 4);
    s_held = (int16_t)((adpcm_decode(nib) - 0x800) << 4);
    if (s_play_nibble) {
        s_read_addr++;
        if (s_length) s_length--;
        if (!s_length && !(s_last_cmd & 0x10)) { s_playing = false; s_end = true; }
    }
    s_play_nibble = !s_play_nibble;
}

void pce_adpcm_write(uint8_t reg, uint8_t val)
{
    switch (reg & 0x0F) {
    case 0x8:                                   /* ADPCM address low */
        if (s_last_cmd & 0x80) break;
        s_addr = (s_addr & 0xFF00) | val;
        if (s_last_cmd & 0x10) s_length = s_addr;
        break;
    case 0x9:                                   /* ADPCM address high */
        if (s_last_cmd & 0x80) break;
        s_addr = (s_addr & 0x00FF) | ((uint16_t)val << 8);
        if (s_last_cmd & 0x10) s_length = s_addr;
        break;
    case 0xA:                                   /* write a byte to ADPCM RAM */
        s_ram[s_write_addr++] = val;
        break;
    case 0xD:                                   /* control */
        if (val & 0x80) {                       /* reset */
            s_addr = s_read_addr = s_write_addr = s_length = 0;
            s_last_cmd = 0;
            s_playing = s_end = s_half = s_play_nibble = false;
            s_cur = 0x800; s_ssi = 0;
            return;
        }
        if (s_playing && !(val & 0x20)) s_playing = false;
        if (!s_playing && (val & 0x20)) {        /* start playback */
            s_playing = true; s_half = false; s_play_nibble = false;
            s_cur = 0x800; s_ssi = 0; s_phase = 0;
        }
        if (val & 0x10) { s_length = s_addr; s_end = false; }
        if (!(s_last_cmd & 0x08) && (val & 0x08)) s_read_addr  = (val & 0x04) ? s_addr : (uint16_t)(s_addr - 1);
        if (!(s_last_cmd & 0x02) && (val & 0x02)) s_write_addr = (val & 0x01) ? s_addr : (uint16_t)(s_addr - 1);
        s_last_cmd = val;
        break;
    case 0xE:                                   /* playback rate */
        s_freq = val & 0x0F;
        break;
    default: break;
    }
}

uint8_t pce_adpcm_read(uint8_t reg)
{
    switch (reg & 0x0F) {
    case 0xA: {                                 /* read a byte from ADPCM RAM.
        * Real MSM5205 interface has a one-read LATENCY: $180A returns the
        * previously latched byte and fetches the next (Mednafen ReadBuffer/
        * ReadPending). The System Card's AD_READ knows this and issues a dummy
        * read to prime the pipe — with our old immediate model that dummy read
        * CONSUMED a real byte, shifting every AD_READ record by one (Dynastic
        * Hero streams its level decompressor through AD_READ: the desynced
        * stream yielded a garbage block count -> I/O spray -> purple screen). */
        uint8_t v = s_read_latch;
        s_read_latch = s_ram[s_read_addr++];
        return v;
    }
    case 0xC:                                   /* status */
        return (uint8_t)((s_end ? 0x01 : 0) | (s_playing ? 0x08 : 0));
    default:
        return 0;
    }
}

/* Called from the SCSI->ADPCM DMA drain: stream one CD byte into ADPCM RAM. */
void pce_adpcm_dma_byte(uint8_t val)
{
    s_ram[s_write_addr++] = val;
}

bool pce_adpcm_playing(void) { return s_playing; }
bool pce_adpcm_end_flag(void) { return s_end; }

/* Savestate: the engine registers + RAM must round-trip, or a loaded state's
 * game-side audio sequencer (restored from game RAM) points at segment
 * addresses/lengths that no longer match the live ADPCM RAM — observed on
 * device as "load, ~2s of music, then the game hangs waiting for a segment
 * that never ends" (Ai Chou Aniki). */
void pce_adpcm_get(uint32_t out[PCE_ADPCM_STATE_WORDS])
{
    out[0] = (uint32_t)s_addr | ((uint32_t)s_read_addr << 16);
    out[1] = (uint32_t)s_write_addr | ((uint32_t)s_length << 16);
    out[2] = (uint32_t)s_last_cmd | ((uint32_t)s_freq << 8) |
             ((uint32_t)(s_playing ? 1 : 0) << 16) | ((uint32_t)(s_end ? 1 : 0) << 17) |
             ((uint32_t)(s_half ? 1 : 0) << 18)    | ((uint32_t)(s_play_nibble ? 1 : 0) << 19);
    out[3] = (uint32_t)s_cur;
    out[4] = (uint32_t)s_ssi;
    out[5] = s_phase;
    out[6] = (uint32_t)(uint16_t)s_held;
    out[7] = (uint32_t)s_read_latch;
}

void pce_adpcm_set(const uint32_t in[PCE_ADPCM_STATE_WORDS])
{
    s_addr        = (uint16_t)(in[0] & 0xFFFF);
    s_read_addr   = (uint16_t)(in[0] >> 16);
    s_write_addr  = (uint16_t)(in[1] & 0xFFFF);
    s_length      = (uint16_t)(in[1] >> 16);
    s_last_cmd    = (uint8_t)(in[2] & 0xFF);
    s_freq        = (uint8_t)((in[2] >> 8) & 0x0F);
    s_playing     = (in[2] >> 16) & 1;
    s_end         = (in[2] >> 17) & 1;
    s_half        = (in[2] >> 18) & 1;
    s_play_nibble = (in[2] >> 19) & 1;
    s_cur         = (int32_t)(in[3] & 0xFFF);
    s_ssi         = (int)in[4]; if (s_ssi < 0 || s_ssi > 48) s_ssi = 0;
    s_phase       = in[5];
    s_held        = (int16_t)(uint16_t)in[6];
    s_read_latch  = (uint8_t)in[7];   /* 0 in old saves: one stale byte, harmless */
}

uint8_t *pce_adpcm_ram(void) { return s_ram; }

/* Fill `frames` stereo int16 samples (mono ADPCM duplicated L/R) at OUT_RATE,
 * decoding at the programmed rate fs = 32087.5/(16-freq). Returns frames if
 * playing, else 0. */
int pce_adpcm_fill(int16_t *out, int frames)
{
    if (!s_playing) return 0;
    /* fs (Hz) * 256 for fixed point: 32087.5*256 ≈ 8214400 */
    uint32_t fs = (uint32_t)(8214400u / (16 - s_freq));   /* fs<<8 */
    uint32_t step = fs / OUT_RATE;                          /* whole part (<<8 cancels below) */
    /* use a <<8 phase accumulator: advance the decoder fs/OUT_RATE nibbles per
     * output sample */
    for (int i = 0; i < frames; i++) {
        s_phase += fs;                                      /* fs is already <<8 */
        while (s_phase >= (OUT_RATE << 8)) {
            adpcm_tick();
            s_phase -= (OUT_RATE << 8);
        }
        out[i * 2] = out[i * 2 + 1] = s_held;
        if (!s_playing) {                                   /* sample ended mid-buffer */
            for (i++; i < frames; i++) { out[i * 2] = out[i * 2 + 1] = 0; }
            return frames;
        }
    }
    (void)step;
    return frames;
}
