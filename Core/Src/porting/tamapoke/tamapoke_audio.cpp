/* Square-wave SFX, ported off the ES8311 + FreeRTOS task.
 *
 * Upstream queues an effect and a dedicated task blocks its way through the
 * notes, writing straight to I2S and toggling the amplifier around each one.
 * There is no RTOS here and the frame loop must not block, so the same note
 * tables are driven by a state machine that the frame loop pulls from.
 *
 * The tables, the amplitude and the attack/decay ramps are upstream's, and the
 * sample rate stays at 16 kHz because those ramps are expressed in samples
 * (64 in, 96 out) and tuned for it.
 */
#include "tamapoke_audio.h"

#include <string.h>

#define SFX_AMPLITUDE 5000
#define ATTACK_SAMPLES 64
#define DECAY_SAMPLES 96

struct Note {
  uint16_t f, ms;
};

static const Note N_TAP[]    = {{880, 35}};
static const Note N_EAT[]    = {{660, 45}, {0, 12}, {660, 45}};
static const Note N_PLAY[]   = {{784, 45}, {988, 60}};
static const Note N_HEART[]  = {{1047, 55}, {1319, 90}};
static const Note N_HATCH[]  = {{523, 80}, {659, 80}, {784, 110}, {1047, 170}};
static const Note N_EVOLVE[] = {{523, 80}, {659, 80}, {784, 80}, {1047, 90}, {1319, 230}};
static const Note N_MEDAL[]  = {{784, 70}, {0, 25}, {784, 70}, {0, 25}, {1047, 200}};
static const Note N_DENY[]   = {{300, 110}, {200, 170}};
static const Note N_BYE[]    = {{784, 150}, {659, 150}, {523, 280}};
static const Note N_LEVEL[]  = {{784, 70}, {1047, 130}};

struct SfxDef {
  const Note *n;
  uint8_t len;
};

static const SfxDef SFX[SFX_COUNT] = {
    {N_TAP, 1},    {N_EAT, 3},   {N_PLAY, 2},  {N_HEART, 2}, {N_HATCH, 4},
    {N_EVOLVE, 5}, {N_MEDAL, 5}, {N_DENY, 2},  {N_BYE, 3},   {N_LEVEL, 2},
};

/* One effect at a time, like the upstream queue depth of one in practice: a
 * new sfxPlay() during playback replaces what is sounding rather than mixing,
 * which is what the single I2S writer did anyway. */
static struct {
  const SfxDef *def;
  uint8_t note;      /* index into def->n */
  int note_total;    /* samples in the current note */
  int note_done;     /* samples emitted of it */
  int phase;         /* square-wave phase counter */
  bool high;
  bool enabled;
} g_sfx = {nullptr, 0, 0, 0, 0, true, true};

static void begin_note(void) {
  const Note &n = g_sfx.def->n[g_sfx.note];
  g_sfx.note_total = TAMAPOKE_SAMPLE_RATE * n.ms / 1000;
  g_sfx.note_done = 0;
  g_sfx.phase = 0;
  g_sfx.high = true;
}

void audioBegin(void) {
  g_sfx.def = nullptr;
  g_sfx.enabled = true;
}

void sfxPlay(uint8_t id) {
  if (!g_sfx.enabled || id >= SFX_COUNT) return;
  g_sfx.def = &SFX[id];
  g_sfx.note = 0;
  begin_note();
}

void audioSetEnabled(bool on) {
  g_sfx.enabled = on;
  if (!on) g_sfx.def = nullptr;
}

bool audioEnabled(void) { return g_sfx.enabled; }

/* Emit one sample of the current note, advancing the square wave.
 * Returns 0 once the effect has run out, which also silences the tail. */
static int16_t next_sample(void) {
  if (!g_sfx.def) return 0;

  const Note &n = g_sfx.def->n[g_sfx.note];
  int16_t s = 0;

  if (n.f) {
    s = g_sfx.high ? SFX_AMPLITUDE : -SFX_AMPLITUDE;
    int idx = g_sfx.note_done;
    if (idx < ATTACK_SAMPLES) {
      s = (int16_t)(s * idx / ATTACK_SAMPLES);
    } else if (idx > g_sfx.note_total - DECAY_SAMPLES) {
      s = (int16_t)(s * (g_sfx.note_total - idx) / DECAY_SAMPLES);
    }
    int half = TAMAPOKE_SAMPLE_RATE / (2 * n.f); /* half period in samples */
    if (++g_sfx.phase >= half) {
      g_sfx.phase = 0;
      g_sfx.high = !g_sfx.high;
    }
  }

  if (++g_sfx.note_done >= g_sfx.note_total) {
    if (++g_sfx.note >= g_sfx.def->len) {
      g_sfx.def = nullptr; /* effect finished */
    } else {
      begin_note();
    }
  }
  return s;
}

void tamapoke_audio_fill(int16_t *dst, int samples) {
  if (!dst || samples <= 0) return;
  if (!g_sfx.def || !g_sfx.enabled) {
    memset(dst, 0, (size_t)samples * sizeof(int16_t));
    return;
  }
  for (int i = 0; i < samples; i++) dst[i] = next_sample();
}
