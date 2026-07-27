/* SFX interface. The names match upstream so the game logic is unmodified;
 * see tamapoke_audio.cpp for why the blocking task became a pull model.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Upstream's rate, kept because the attack/decay ramps are expressed in
 * samples and tuned for it. */
#define TAMAPOKE_SAMPLE_RATE 16000
#define TAMAPOKE_FPS 30
#define TAMAPOKE_AUDIO_BUFFER_LENGTH (TAMAPOKE_SAMPLE_RATE / TAMAPOKE_FPS)

/* Order matches the SFX table and is relied on by the game logic. */
enum Sfx : uint8_t {
  SFX_TAP = 0,
  SFX_EAT,
  SFX_PLAY,
  SFX_HEART,
  SFX_HATCH,
  SFX_EVOLVE,
  SFX_MEDAL,
  SFX_DENY,
  SFX_BYE,
  SFX_LEVEL,
  SFX_COUNT
};

void audioBegin(void);
void sfxPlay(uint8_t id);
void audioSetEnabled(bool on);
bool audioEnabled(void);

/* Pulled once per frame by the port. Writes exactly `samples` mono samples,
 * zero-filling when nothing is sounding. */
void tamapoke_audio_fill(int16_t *dst, int samples);
