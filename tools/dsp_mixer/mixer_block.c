/* S-DSP block mixer: dspb_run(dsp, n) == n * dsp_cycle(dsp), bit-identically,
 * but restructured voice-major instead of sample-major.
 *
 * Why this can be exact (not approximate):
 *  - KON/KOF take effect inside dsp_write (MY_CHANGES=1 in dsp.c), so they only
 *    happen at block boundaries -- the caller splits runs at every register write.
 *  - PMON couples voice ch to ch-1's output of the SAME sample; voices are
 *    processed in order 0..7 with each voice's per-sample outputs kept in a row
 *    buffer, so voice ch reads out[ch-1][k] -- exactly what the reference read.
 *  - The noise LFSR evolves independently of the voices; its per-sample values
 *    are precomputed for the chunk with the reference's exact step.
 *  - Per-channel accumulation clamps: skipping an all-zero row is identity
 *    (the running total is already clamped into range; adding 0 changes nothing).
 *  - A released, silent voice (adsrState==4, gain==0, no PMON) provably outputs
 *    0 and evolves ONLY its pitch counter / BRR decode chain (release ticks
 *    leave gain at 0, rateCounter untouched) -- so it is skipped ahead decode to
 *    decode in O(1) per BRR block, side effects (ENDx, decodeOffset) preserved.
 *
 * The BRR decoder, envelope steps, echo FIR and every clamp/clip/truncation are
 * verbatim transcriptions of external/sm/src/snes/dsp.c (GNW_SNES_CORE linear
 * interpolation build). The gate in mixer_ab.c compares the full Dsp state, the
 * 64 KB ARAM (echo writes) and all 534 samples per frame against the reference.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "src/snes/dsp.h"

#define CHUNK 256

/* ---- verbatim from dsp.c: BRR block decode ------------------------------- */
static void blk_decodeBrr(Dsp* dsp, int ch) {
  dsp->channel[ch].decodeBuffer[0] = dsp->channel[ch].decodeBuffer[16];
  dsp->channel[ch].decodeBuffer[1] = dsp->channel[ch].decodeBuffer[17];
  dsp->channel[ch].decodeBuffer[2] = dsp->channel[ch].decodeBuffer[18];
  if(dsp->channel[ch].previousFlags == 1 || dsp->channel[ch].previousFlags == 3) {
    uint16_t samplePointer = dsp->dirPage + 4 * dsp->channel[ch].srcn;
    dsp->channel[ch].decodeOffset = dsp->apu_ram[(samplePointer + 2) & 0xffff];
    dsp->channel[ch].decodeOffset |= (dsp->apu_ram[(samplePointer + 3) & 0xffff]) << 8;
    if(dsp->channel[ch].previousFlags == 1) {
      dsp->channel[ch].adsrState = 4;
      dsp->channel[ch].gain = 0;
    }
    dsp->ram[0x7c] |= 1 << ch;
  }
  uint8_t header = dsp->apu_ram[dsp->channel[ch].decodeOffset++];
  int shift = header >> 4;
  int filter = (header & 0xc) >> 2;
  dsp->channel[ch].previousFlags = header & 0x3;
  uint8_t curByte = 0;
  int old = dsp->channel[ch].old;
  int older = dsp->channel[ch].older;
  for(int i = 0; i < 16; i++) {
    int s = 0;
    if(i & 1) {
      s = curByte & 0xf;
    } else {
      curByte = dsp->apu_ram[dsp->channel[ch].decodeOffset++];
      s = curByte >> 4;
    }
    if(s > 7) s -= 16;
    if(shift <= 0xc) {
      s = (s << shift) >> 1;
    } else {
      s = (s >> 3) << 12;
    }
    switch(filter) {
      case 1: s += old + (-old >> 4); break;
      case 2: s += 2 * old + ((3 * -old) >> 5) - older + (older >> 4); break;
      case 3: s += 2 * old + ((13 * -old) >> 6) - older + ((3 * older) >> 4); break;
    }
    s = s < -0x8000 ? -0x8000 : (s > 0x7fff ? 0x7fff : s);
    s = ((int16_t) ((s & 0x7fff) << 1)) >> 1;
    older = old;
    old = s;
    dsp->channel[ch].decodeBuffer[i + 3] = s;
  }
  dsp->channel[ch].older = older;
  dsp->channel[ch].old = old;
}

/* ---- verbatim from dsp.c: envelope step (on locals) ----------------------- */
static inline void blk_gainStep(uint8_t *state, uint16_t *gain,
                                const uint16_t rates[4], uint16_t sustainLevel,
                                uint8_t gainMode) {
  switch(*state) {
    case 0: {
      uint16_t rate = rates[0];
      *gain += rate == 1 ? 1024 : 32;
      if(*gain >= 0x7e0) *state = 1;
      if(*gain > 0x7ff) *gain = 0x7ff;
      break;
    }
    case 1: {
      *gain -= ((*gain - 1) >> 8) + 1;
      if(*gain < sustainLevel) *state = 2;
      break;
    }
    case 2: {
      *gain -= ((*gain - 1) >> 8) + 1;
      break;
    }
    case 3: {
      switch(gainMode) {
        case 0: {
          *gain -= 32;
          if(*gain > 0x7ff) *gain = 0;
          break;
        }
        case 1: {
          *gain -= ((*gain - 1) >> 8) + 1;
          break;
        }
        case 2: {
          *gain += 32;
          if(*gain > 0x7ff) *gain = 0x7ff;
          break;
        }
        case 3: {
          *gain += *gain < 0x600 ? 32 : 8;
          if(*gain > 0x7ff) *gain = 0x7ff;
          break;
        }
      }
      break;
    }
    case 4: {
      *gain -= 8;
      if(*gain > 0x7ff) *gain = 0;
      break;
    }
  }
}

/* ---- verbatim from dsp.c: echo, reading this sample's voice outputs ------- */
static void blk_echo(Dsp* dsp, int* outputL, int* outputR,
                     const int16_t *voiceOut, int stride, const bool rowNonZero[8]) {
  uint16_t adr = dsp->echoBufferAdr + dsp->echoBufferIndex * 4;
  dsp->firBufferL[dsp->firBufferIndex] = (
    dsp->apu_ram[adr] + (dsp->apu_ram[(adr + 1) & 0xffff] << 8)
  );
  dsp->firBufferL[dsp->firBufferIndex] >>= 1;
  dsp->firBufferR[dsp->firBufferIndex] = (
    dsp->apu_ram[(adr + 2) & 0xffff] + (dsp->apu_ram[(adr + 3) & 0xffff] << 8)
  );
  dsp->firBufferR[dsp->firBufferIndex] >>= 1;
  int sumL = 0, sumR = 0;
  for(int i = 0; i < 8; i++) {
    sumL += (dsp->firBufferL[(dsp->firBufferIndex + i + 1) & 0x7] * dsp->firValues[i]) >> 6;
    sumR += (dsp->firBufferR[(dsp->firBufferIndex + i + 1) & 0x7] * dsp->firValues[i]) >> 6;
    if(i == 6) {
      sumL = ((int16_t) (sumL & 0xffff));
      sumR = ((int16_t) (sumR & 0xffff));
    }
  }
  sumL = sumL < -0x8000 ? -0x8000 : (sumL > 0x7fff ? 0x7fff : sumL);
  sumR = sumR < -0x8000 ? -0x8000 : (sumR > 0x7fff ? 0x7fff : sumR);
  int outL = *outputL + ((sumL * dsp->echoVolumeL) >> 7);
  int outR = *outputR + ((sumR * dsp->echoVolumeR) >> 7);
  *outputL = outL < -0x8000 ? -0x8000 : (outL > 0x7fff ? 0x7fff : outL);
  *outputR = outR < -0x8000 ? -0x8000 : (outR > 0x7fff ? 0x7fff : outR);
  int inL = 0, inR = 0;
  for(int i = 0; i < 8; i++) {
    if(dsp->channel[i].echoEnable && rowNonZero[i]) {
      int16_t vo = voiceOut[i * stride];
      inL += (vo * dsp->channel[i].volumeL) >> 6;
      inR += (vo * dsp->channel[i].volumeR) >> 6;
      inL = inL < -0x8000 ? -0x8000 : (inL > 0x7fff ? 0x7fff : inL);
      inR = inR < -0x8000 ? -0x8000 : (inR > 0x7fff ? 0x7fff : inR);
    }
  }
  inL += (sumL * dsp->feedbackVolume) >> 7;
  inR += (sumR * dsp->feedbackVolume) >> 7;
  inL = inL < -0x8000 ? -0x8000 : (inL > 0x7fff ? 0x7fff : inL);
  inR = inR < -0x8000 ? -0x8000 : (inR > 0x7fff ? 0x7fff : inR);
  inL &= 0xfffe;
  inR &= 0xfffe;
  if(dsp->echoWrites) {
    dsp->apu_ram[adr] = inL & 0xff;
    dsp->apu_ram[(adr + 1) & 0xffff] = inL >> 8;
    dsp->apu_ram[(adr + 2) & 0xffff] = inR & 0xff;
    dsp->apu_ram[(adr + 3) & 0xffff] = inR >> 8;
  }
  dsp->firBufferIndex++;
  dsp->firBufferIndex &= 7;
  dsp->echoBufferIndex++;
  dsp->echoRemain--;
  if(dsp->echoRemain == 0) {
    dsp->echoRemain = dsp->echoDelay;
    dsp->echoBufferIndex = 0;
  }
}

/* ---- one voice, one chunk -------------------------------------------------
 * Fills out[0..K-1] with the voice's post-gain samples; returns whether any
 * sample was non-zero. pmonRow = previous voice's row (NULL for ch 0 / PMON off). */
static bool blk_voice(Dsp *dsp, int ch, int K, int16_t *out,
                      const int16_t *pmonRow, const int16_t *noiseBuf) {
  DspChannel *c = &dsp->channel[ch];
  const bool pm = (ch > 0) && c->pitchModulation && pmonRow != NULL;
  const bool useNoise = c->useNoise;
  const bool reset = dsp->reset;
  const bool useGain = c->useGain;
  const bool directGain = c->directGain;
  const uint16_t gainValue = c->gainValue;
  const uint16_t sustainLevel = c->sustainLevel;
  const uint8_t gainMode = c->gainMode;
  uint16_t rates[4];
  memcpy(rates, c->adsrRates, sizeof(rates));

  uint8_t state = c->adsrState;
  uint16_t gain = c->gain;
  uint16_t rc = c->rateCounter;
  uint32_t pc = c->pitchCounter;
  const uint16_t basePitch = c->pitch;

  /* Released-and-silent fast path: output provably all-zero; only the pitch
   * counter / BRR decode chain evolves (release keeps gain at 0, rateCounter is
   * only touched when state != 4). PMON voices take the slow path: their pitch
   * varies per sample. reset only forces state=4/gain=0 -- already true here. */
  if (state == 4 && gain == 0 && !pm) {
    memset(out, 0, K * sizeof(int16_t));
    if (basePitch != 0) {
      int k = 0;
      while (k < K) {
        int m = (int)((0xffffu - pc) / basePitch) + 1;   /* samples to overflow */
        if (m > K - k) { pc = (pc + (uint32_t)(K - k) * basePitch) & 0xffff; break; }
        k += m;
        pc = (pc + (uint32_t)m * basePitch) & 0xffff;
        blk_decodeBrr(dsp, ch);
      }
    }
    c->pitchCounter = (uint16_t)pc;
    /* reference writes these every sample; final values are what remains */
    dsp->ram[(ch << 4) | 8] = 0;
    dsp->ram[(ch << 4) | 9] = 0;
    c->sampleOut = 0;
    return false;
  }

  bool nz = false;
  int16_t sample = 0;
  for (int k = 0; k < K; k++) {
    /* pitch (+ pitch modulation from previous voice's same-sample output) */
    uint16_t pitch = basePitch;
    if (pm) {
      int factor = (pmonRow[k] >> 4) + 0x400;
      pitch = ((int)basePitch * factor) >> 10;   /* uint16 truncation as reference */
      if (pitch > 0x3fff) pitch = 0x3fff;
    }
    uint32_t nc = pc + pitch;
    if (nc > 0xffff) {
      /* a BRR end-block (previousFlags==1) releases the voice inside the decode
       * (adsrState=4, gain=0 on the struct) -- mirror it into our locals, or the
       * writeback below would clobber the release. Reference order: decode ->
       * release -> sample read -> envelope. */
      uint8_t pfBefore = c->previousFlags;
      blk_decodeBrr(dsp, ch);
      if (pfBefore == 1) { state = 4; gain = 0; }
    }
    pc = nc & 0xffff;

    if (useNoise) {
      sample = noiseBuf[k];
    } else if (gain == 0 && state == 4) {
      sample = 0;
    } else {
      int sn = pc >> 12, off = (pc >> 4) & 0xff;
      int16_t olds = c->decodeBuffer[sn + 2];
      int16_t news = c->decodeBuffer[sn + 3];
      sample = (int16_t)(olds + (((news - olds) * off) >> 8));
    }

    if (reset) { state = 4; gain = 0; }

    bool ddg = state != 4 && useGain && directGain;
    uint16_t rate = state == 4 ? 0 : rates[state];
    if (state != 4 && !ddg && rate != 0) rc++;
    if (state == 4 || (!ddg && rc >= rate && rate != 0)) {
      if (state != 4) rc = 0;
      blk_gainStep(&state, &gain, rates, sustainLevel, gainMode);
    }
    if (ddg) gain = gainValue;

    sample = (int16_t)((sample * gain) >> 11);
    out[k] = sample;
    nz |= (sample != 0);
  }

  c->adsrState = state;
  c->gain = gain;
  c->rateCounter = rc;
  c->pitchCounter = (uint16_t)pc;
  c->sampleOut = sample;
  /* reference writes the mirrors every sample; keep the final values */
  dsp->ram[(ch << 4) | 8] = gain >> 4;
  dsp->ram[(ch << 4) | 9] = sample >> 7;
  return nz;
}

void dspb_run(Dsp *dsp, int n) {
  int16_t out[8][CHUNK];
  int16_t noiseBuf[CHUNK];
  bool rowNonZero[8];

  while (n > 0) {
    int K = n > CHUNK ? CHUNK : n;

    /* noise values each voice sees at sample k (state advances AFTER the
     * voices each sample in the reference; precomputing commutes because
     * nothing else touches the noise state) */
    for (int k = 0; k < K; k++) {
      noiseBuf[k] = dsp->noiseSample;
      if (dsp->noiseRate != 0) {
        dsp->noiseCounter++;
        if (dsp->noiseCounter >= dsp->noiseRate) {
          int bit = (dsp->noiseSample & 1) ^ ((dsp->noiseSample >> 1) & 1);
          dsp->noiseSample = ((dsp->noiseSample >> 1) & 0x3fff) | (bit << 14);
          dsp->noiseSample = ((int16_t) ((dsp->noiseSample & 0x7fff) << 1)) >> 1;
          dsp->noiseCounter = 0;
        }
      }
    }

    /* voices, in order -- voice ch's PMON reads voice ch-1's row */
    for (int ch = 0; ch < 8; ch++)
      rowNonZero[ch] = blk_voice(dsp, ch, K, out[ch],
                                 ch > 0 ? out[ch - 1] : NULL, noiseBuf);

    /* mixdown + echo, per sample (echo FIR/RAM is inherently serial).
     * Hoist the active-voice set: only voices with a non-zero row contribute
     * (+0 then clamp == identity), and volumes are write-gated chunk constants. */
    const int8_t mvl = dsp->masterVolumeL, mvr = dsp->masterVolumeR;
    const bool mute = dsp->mute;
    int nact = 0;
    int actIdx[8]; int actVL[8], actVR[8]; const int16_t *actRow[8];
    for (int i = 0; i < 8; i++) {
      if (!rowNonZero[i]) continue;
      actIdx[nact] = i;
      actVL[nact] = dsp->channel[i].volumeL;
      actVR[nact] = dsp->channel[i].volumeR;
      actRow[nact] = out[i];
      nact++;
    }
    uint16_t off = dsp->sampleOffset;
    for (int k = 0; k < K; k++) {
      int totalL = 0, totalR = 0;
      for (int a = 0; a < nact; a++) {
        int16_t vo = actRow[a][k];
        totalL += (vo * actVL[a]) >> 6;
        totalR += (vo * actVR[a]) >> 6;
        totalL = totalL < -0x8000 ? -0x8000 : (totalL > 0x7fff ? 0x7fff : totalL);
        totalR = totalR < -0x8000 ? -0x8000 : (totalR > 0x7fff ? 0x7fff : totalR);
      }
      totalL = (totalL * mvl) >> 7;
      totalR = (totalR * mvr) >> 7;
      totalL = totalL < -0x8000 ? -0x8000 : (totalL > 0x7fff ? 0x7fff : totalL);
      totalR = totalR < -0x8000 ? -0x8000 : (totalR > 0x7fff ? 0x7fff : totalR);
      blk_echo(dsp, &totalL, &totalR, &out[0][k], CHUNK, rowNonZero);
      if (mute) { totalL = 0; totalR = 0; }
      if (off < 534) {
        dsp->sampleBuffer[off * 2] = totalL;
        dsp->sampleBuffer[off * 2 + 1] = totalR;
        off++;
      }
    }
    dsp->sampleOffset = off;
    if (K & 1) dsp->evenCycle = !dsp->evenCycle;

    n -= K;
  }
}
