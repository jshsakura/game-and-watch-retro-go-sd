#pragma once
/*
 * Z80 + YM2151 + OKI6295 sound HLE player -- skeleton.
 *
 * docs/CPS1_SENIOR_TRICKS_ANALYSIS.md "cheat A": don't emulate the Z80 CPU
 * or the YM2151 FM chip at all. On real hardware the 68000 writes a
 * command byte to a shared sound-command latch and the Z80 reads it and
 * dispatches its music driver; here cps1_core.c's bus intercepts that same
 * write (see CPS1_SOUND_CMD_BASE) and this file interprets the command
 * directly in native C, synthesizing the result with a cheap phase-
 * accumulator mixer instead of a cycle-accurate Z80+FM-chip model.
 *
 * SKELETON, NOT the real CPS-1 sound protocol: the command-byte encoding
 * in cps1_sound_hle_trigger() is invented for this skeleton, not reverse-
 * engineered from a real CPS-1 sound driver -- that needs a real Z80
 * program to disassemble, which doesn't exist yet (same class of gap as
 * the GFX bitplane layout and BG tilemap dimensions: flagged here, not
 * guessed at with false confidence). The tone channels stand in for
 * YM2151 FM channels (a phase-accumulator square wave, not FM synthesis);
 * the OKI channel stands in for OKI6295 ADPCM (raw PCM8 playback, no
 * ADPCM decode implemented yet).
 */
#include <stdint.h>

#define CPS1_SOUND_TONE_CHANNELS 4
#define CPS1_SOUND_SAMPLE_RATE   22050u
/* Samples per 60fps video frame at the mixer's own rate -- moved here from a
 * cps1_core.c-local macro so a device audio bridge could size its
 * audio_start_playing() call without reaching into cps1_core.c internals.
 * THERE IS NO SUCH BRIDGE: this whole mixer is host-only (linux/Makefile.cps1),
 * main_cps1.c never links it, and CPS-1 runs SILENT on the device -- the QSound
 * side is two stub bytes so the game's boot code stops spinning
 * (cps1_sound_stub_init). */
#define CPS1_SOUND_SAMPLES_PER_FRAME (CPS1_SOUND_SAMPLE_RATE / 60u) /* ~368 @ 60fps */

typedef struct {
    uint8_t enabled;
    uint32_t phase;             /* Q0.32 phase accumulator; one full wrap = one cycle */
    uint32_t phase_step;        /* Q0.32 per-sample phase increment (== frequency) */
    int16_t amplitude;          /* peak amplitude of the square wave */
    uint32_t samples_remaining; /* note duration, in samples; channel auto-disables at 0 */
} cps1_sound_tone_channel_t;

typedef struct {
    uint8_t enabled;
    const int8_t *data; /* raw PCM8 samples -- NOT ADPCM-decoded, see file header */
    uint32_t length;
    uint32_t position;
} cps1_sound_oki_channel_t;

typedef struct {
    cps1_sound_tone_channel_t tone[CPS1_SOUND_TONE_CHANNELS];
    cps1_sound_oki_channel_t oki;
} cps1_sound_hle_t;

void cps1_sound_hle_reset(cps1_sound_hle_t *snd);

/* Attaches the OKI-channel sample source. `data` must outlive `snd`. */
void cps1_sound_hle_attach_oki_sample(cps1_sound_hle_t *snd, const int8_t *data, uint32_t length);

/*
 * Interprets one sound command byte (skeleton encoding, see file header):
 *   0x00        : silence all tone channels
 *   0x01..0x7F  : play a tone -- bits[1:0] select the tone channel,
 *                 bits[6:2] index a fixed note table (freq = 220 + note*20
 *                 Hz, linear -- not musically tuned, just deterministic),
 *                 fixed amplitude and a 250ms duration
 *   0x80..0xFF  : (re)trigger the OKI sample from the start
 */
void cps1_sound_hle_trigger(cps1_sound_hle_t *snd, uint8_t command);

/* Mixes `count` samples of every active channel (additive, clipped to
 * int16_t) into `out` (mono, overwrites -- does not accumulate onto
 * existing content). Advances every channel's internal state so repeated
 * calls continue where the last one left off; channels disable themselves
 * once their note/sample ends. */
void cps1_sound_hle_mix(cps1_sound_hle_t *snd, int16_t *out, uint32_t count);
