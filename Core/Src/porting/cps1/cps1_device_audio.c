/*
 * Device-only bridge: cps1_sound_hle_mix() (via cps1_core_sound_mix(),
 * cps1_core.h) -> the real SAI DMA double-buffer (Core/Src/gw_audio.c),
 * the SAME audio sink every other emulator core in this repo uses
 * (docs/CPS1_MAME_ALIGNMENT.md section 9, Phase 12 plan: "the existing,
 * proven device audio sink for every other core in this repo -- reuse it,
 * don't reinvent it"). Mirrors Core/Src/porting/gba/main_gba.c's own
 * audio_get_active_buffer()/audio_get_buffer_length() call pattern
 * exactly (gba_pcm_submit(), main_gba.c:444-485).
 *
 * ARM-only, NOT freestanding: gw_audio.h pulls in the real SAI_
 * HandleTypeDef/STM32 HAL types cps1_core.c deliberately never references
 * (freestanding on purpose, host+device portable) -- this file is the one
 * place that bridge happens, same layering every other
 * Core/Src/porting/<system>/main_<system>.c uses for its own audio-submit
 * call site. Not part of linux/Makefile.cps1's host harness (cannot
 * compile there -- no STM32 HAL on Linux); build-verified separately
 * against the real device toolchain/headers (see the Phase 12 commit
 * message for the exact command and its result).
 *
 * cps1_sound_hle_mix() (and therefore cps1_core_sound_mix()) produces
 * MONO int16 samples -- CPS-1's real sound path is stereo (this skeleton
 * doesn't model panning at all yet), but Game & Watch hardware has a
 * single physical speaker, the same reason every other core's audio path
 * (e.g. GBA's gba_pcm_submit(), which explicitly folds stereo to mono)
 * ends up mono here regardless of the source system's own channel count.
 */
#include "gw_audio.h"
#include "cps1_core.h"
#include "cps1_sound_hle.h"

/* Starts SAI DMA circular playback sized to one video frame's worth of
 * samples at the mixer's own rate (odroid_system_init's sampleRate
 * argument, at boot, must already have been set to CPS1_SOUND_SAMPLE_RATE
 * -- this function only starts the DMA, matching gba_Start()'s own
 * odroid_system_init(...) + audio_start_playing(...) sequence). */
void cps1_device_audio_start(void)
{
    audio_start_playing(CPS1_SOUND_SAMPLES_PER_FRAME);
}

/* Call once per video frame (mirrors gba_pcm_submit()'s own call site):
 * fills whichever DMA half is currently safe to write with freshly-mixed
 * samples. audio_get_buffer_length() returns that half's real sample
 * count (which alternates by 1 around CPS1_SOUND_SAMPLES_PER_FRAME when
 * the frame's sample count is odd, per gw_audio.c's audio_get_buffer_
 * length()) -- cps1_sound_hle_mix() has no fixed-size internal buffer, so
 * any count is safe, no clamping needed (unlike gba_pcm_submit(), which
 * clamps against its OWN fixed-size stereo scratch buffer -- this mixer
 * has none). */
void cps1_device_audio_submit(void)
{
    int16_t *out = audio_get_active_buffer();
    uint16_t len = audio_get_buffer_length();
    cps1_core_sound_mix(out, len);
}
