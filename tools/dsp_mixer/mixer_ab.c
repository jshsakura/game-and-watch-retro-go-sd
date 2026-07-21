/* S-DSP block-mixer A/B: proves dsp_runBlock() (src/snes/dsp_block.c) is bit-identical to
 * the reference dsp_cycle() loop on real game audio, then times both.
 *
 * How: run the real emulation (65816 + SPC700 + reference DSP, the survey
 * harness's parity-checked event loop). apu.c is compiled with
 *   -Ddsp_cycle=hook_dsp_cycle -Ddsp_write=hook_dsp_write
 * so every DSP cycle and register write passes through this file, which records,
 * per frame: a snapshot (Dsp struct + 64 KB ARAM) taken at frame start, the
 * cycle count, and the write stream tagged with its cycle position.
 *
 * Each frame is then replayed OFFLINE twice from the snapshot -- reference
 * dsp_cycle loop vs dsp_runBlock -- on identical inputs (own ARAM copy each, echo
 * writes included), and compared: full Dsp state (minus the apu_ram pointer),
 * the 64 KB ARAM, and all 534 stored samples. That is the gate.
 *
 * (The offline reference can differ from the LIVE reference only where the
 * SPC700 wrote ARAM mid-frame under the DSP's reads -- reported as info, not
 * gated: it does not bear on mixer correctness, which is ref-vs-block on the
 * same inputs.)
 *
 * Usage: mixer_ab <rom> [frames] [bench-reps]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "src/snes/snes.h"
#include "src/snes/cart.h"
#include "src/snes/ppu.h"
#include "src/snes/apu.h"
#include "src/snes/cpu.h"
#include "src/snes/dma.h"
#include "src/snes/input.h"

bool snes_loadRom(Snes* snes, const uint8_t* data, int length);
void dsp_runBlock(Dsp *dsp, int n);   /* external/sm/src/snes/dsp_block.c */
#ifdef DSP_MIXER_DIAG
extern uint64_t dspb_diag_block_samples, dspb_diag_ref_samples;
extern uint64_t dspb_diag_hazard_chunks, dspb_diag_pmon_echo_chunks;
void dspb_diag_reset(void);
#endif

/* firmware allocator shims */
void *itc_calloc(size_t n, size_t s) { return calloc(n, s); }
void *itc_malloc(size_t s) { return malloc(s); }
void *ahb_malloc(size_t s) { return malloc(s); }
void *ram_malloc(size_t s) { return malloc(s); }
void *ram_calloc(size_t n, size_t s) { return calloc(n, s); }

int  CpuOpcodeHook(uint32_t addr) { (void)addr; return 0; }
bool HookedFunctionRts(int level) { (void)level; return false; }
bool g_fail;
bool g_new_ppu = true;

void Die(const char *s) { printf("Die: %s\n", s); exit(1); }
void Warning(const char *s) { (void)s; }

static Snes *g_the_snes;
void RtlApuWrite(uint32_t adr, uint8_t val) {
  snes_catchupApu(g_the_snes);
  g_the_snes->apu->inPorts[adr & 0x3] = val;
}

static uint8_t  g_wram[0x20000];
static uint16_t g_fb[320 * 240];

static const double apuCyclesPerMaster = (32040 * 32) / (1364 * 262 * 60.0);

/* ---- event loop, verbatim from snes_survey.c (the parity oracle) ---------- */
static int dots_to_next_event(Snes *snes) {
  int h = snes->hPos;
  if (h == 0 || h == 512 || h == 1024) return 0;
  if (snes->hIrqEnabled && h == snes->hTimer * 4) return 0;
  int next = 1362;
  if (h < 512)       next = 512;
  else if (h < 1024) next = 1024;
  if (snes->hIrqEnabled) {
    int t = snes->hTimer * 4;
    if (t > h && t < next) next = t;
  }
  return next - h;
}
static void apply_irq_match(Snes *snes) {
  if (!(snes->hIrqEnabled || snes->vIrqEnabled)) return;
  if (snes->vIrqEnabled && snes->vPos != snes->vTimer) return;
  if (snes->hIrqEnabled && snes->hPos != snes->hTimer * 4) return;
  snes->inIrq = true;
  snes->cpu->irqWanted = true;
}
static void cpu_tick(Snes *snes) {
  if (dma_cycle(snes->dma)) return;
  if (snes->cpuCyclesLeft == 0) {
    snes->cpuMemOps = 0;
    int cycles = cpu_runOpcode(snes->cpu);
    snes->cpuCyclesLeft += (cycles - snes->cpuMemOps) * 6;
  }
  snes->cpuCyclesLeft -= 2;
}
static void run_dots(Snes *snes, int dots) {
  while (dots > 0) {
    if (snes->dma->dmaBusy || snes->dma->hdmaTimer > 0) {
      dma_cycle(snes->dma);
      snes->apuCatchupCycles += apuCyclesPerMaster * 2.0;
      snes->hPos += 2; dots -= 2; continue;
    }
    bool started_dma = false;
    if (snes->cpuCyclesLeft == 0) {
      apply_irq_match(snes);
      snes->cpuMemOps = 0;
      int cycles = cpu_runOpcode(snes->cpu);
      snes->cpuCyclesLeft += (cycles - snes->cpuMemOps) * 6;
      started_dma = snes->dma->dmaBusy || snes->dma->hdmaTimer > 0;
    }
    int step;
    if (snes->cpuCyclesLeft >= 2 && !started_dma) {
      step = snes->cpuCyclesLeft;
      if (step > dots) step = dots;
      step &= ~1;
      snes->cpuCyclesLeft -= (uint8_t)step;
    } else {
      step = 2;
      snes->cpuCyclesLeft -= 2;
    }
    snes->apuCatchupCycles += apuCyclesPerMaster * step;
    snes->hPos += step; dots -= step;
  }
}
static void run_frame_events(Snes *snes) {
  for (;;) {
    snes->apuCatchupCycles += apuCyclesPerMaster * 2.0;
    snes_handle_pos_stuff(snes);
    cpu_tick(snes);
    if (snes->hPos == 0 && snes->vPos == 0) break;
    run_dots(snes, dots_to_next_event(snes));
  }
  snes_catchupApu(snes);
}

/* ---- capture --------------------------------------------------------------- */
typedef struct { int32_t cyc; uint8_t adr, val; } WriteRec;

typedef struct {
  Dsp     snap;                 /* Dsp state at frame start (apu_ram ptr stale) */
  uint8_t *ramSnap;             /* 64 KB ARAM at frame start */
  int32_t ncycles;              /* dsp_cycles this frame (incl. drain) */
  int32_t wfirst, wcount;       /* into the global write pool */
  int16_t liveBuf[534 * 2];     /* what the live reference stored (info only) */
} FrameRec;

#define MAX_FRAMES 4000
#define MAX_WRITES (1 << 20)
static FrameRec g_frames[MAX_FRAMES];
static WriteRec g_writes[MAX_WRITES];
static int g_nframes = 0, g_nwrites = 0;

static int32_t g_cyc = 0;       /* dsp_cycles so far this frame */
static bool g_capturing = false;

/* the -D renames in apu.c land here */
void hook_dsp_cycle(Dsp *dsp) { g_cyc++; dsp_cycle(dsp); }
void hook_dsp_write(Dsp *dsp, uint8_t adr, uint8_t val) {
  if (g_capturing && g_nwrites < MAX_WRITES) {
    g_writes[g_nwrites].cyc = g_cyc;
    g_writes[g_nwrites].adr = adr;
    g_writes[g_nwrites].val = val;
    g_nwrites++;
  }
  dsp_write(dsp, adr, val);
}

/* ---- offline replay --------------------------------------------------------
 * engine 0 = reference dsp_cycle loop, 1 = block mixer */
static void replay(const FrameRec *fr, Dsp *inst, uint8_t *ram, int engine) {
  memcpy(inst, &fr->snap, sizeof(Dsp));
  memcpy(ram, fr->ramSnap, 0x10000);
  inst->apu_ram = ram;
  int32_t done = 0;
  int w = fr->wfirst;
  const int wend = fr->wfirst + fr->wcount;
  while (done < fr->ncycles || w < wend) {
    int32_t next = (w < wend) ? g_writes[w].cyc : fr->ncycles;
    if (next > fr->ncycles) next = fr->ncycles;
    int32_t run = next - done;
    if (run > 0) {
      if (engine == 0) { for (int32_t i = 0; i < run; i++) dsp_cycle(inst); }
      else             dsp_runBlock(inst, run);
      done = next;
    }
    while (w < wend && g_writes[w].cyc <= done) {
      dsp_write(inst, g_writes[w].adr, g_writes[w].val);
      w++;
    }
    if (run <= 0 && done >= fr->ncycles && w >= wend) break;
  }
}

/* Bisect one frame: find the first (segment, sample) where block diverges from
 * ref. Replays ref per-sample and block chunked to k samples, comparing state. */
static void dump_ch(const char *tag, const Dsp *d, int ch) {
  const DspChannel *c = &d->channel[ch];
  printf("  %s ch%d: state=%d gain=%04x rc=%04x pc=%04x pitch=%04x srcn=%02x decOff=%04x pf=%d "
         "useGain=%d dg=%d gv=%04x gm=%d sus=%04x noise=%d pmon=%d out=%d\n",
         tag, ch, c->adsrState, c->gain, c->rateCounter, c->pitchCounter, c->pitch, c->srcn,
         c->decodeOffset, c->previousFlags, c->useGain, c->directGain, c->gainValue, c->gainMode,
         c->sustainLevel, c->useNoise, c->pitchModulation, c->sampleOut);
}
static void debug_frame(const FrameRec *fr) {
  static Dsp R, B;
  uint8_t *rr = malloc(0x10000), *rb = malloc(0x10000);
  const size_t cmpOff = offsetof(Dsp, ram);
  /* lockstep by write segment */
  memcpy(&R, &fr->snap, sizeof(Dsp)); memcpy(rr, fr->ramSnap, 0x10000); R.apu_ram = rr;
  memcpy(&B, &fr->snap, sizeof(Dsp)); memcpy(rb, fr->ramSnap, 0x10000); B.apu_ram = rb;
  int32_t done = 0; int w = fr->wfirst; const int wend = fr->wfirst + fr->wcount;
  int seg = 0;
  while (done < fr->ncycles || w < wend) {
    int32_t next = (w < wend) ? g_writes[w].cyc : fr->ncycles;
    if (next > fr->ncycles) next = fr->ncycles;
    int32_t run = next - done;
    if (run > 0) {
      /* bisect within the segment: advance one sample at a time on both */
      for (int32_t k = 0; k < run; k++) {
        Dsp saveB; memcpy(&saveB, &B, sizeof(Dsp));
        dsp_cycle(&R);
        dsp_runBlock(&B, 1);
        if (memcmp((uint8_t*)&R + cmpOff, (uint8_t*)&B + cmpOff, sizeof(Dsp) - cmpOff) ||
            memcmp(rr, rb, 0x10000)) {
          printf("first divergence: segment %d, sample %d of %d (abs cycle %d)\n",
                 seg, (int)k, (int)run, (int)(done + k));
          const uint8_t *a=(const uint8_t*)&R+cmpOff, *b=(const uint8_t*)&B+cmpOff;
          for (size_t o = 0; o < sizeof(Dsp)-cmpOff; o++)
            if (a[o]!=b[o]) { printf("  struct+%zu: ref=%02x blk=%02x\n", o, a[o], b[o]); if (o>40) break; }
          for (int ch = 0; ch < 8; ch++) { dump_ch("ref", &R, ch); dump_ch("blk", &B, ch); }
          printf("  pre-divergence blk state:\n");
          for (int ch = 0; ch < 8; ch++) dump_ch("pre", &saveB, ch);
          printf("  noise ref: s=%d cnt=%d rate=%d | blk: s=%d cnt=%d rate=%d | reset r=%d b=%d\n",
                 R.noiseSample, R.noiseCounter, R.noiseRate, B.noiseSample, B.noiseCounter, B.noiseRate,
                 R.reset, B.reset);
          free(rr); free(rb); return;
        }
      }
      done = next;
    }
    while (w < wend && g_writes[w].cyc <= done) {
      dsp_write(&R, g_writes[w].adr, g_writes[w].val);
      dsp_write(&B, g_writes[w].adr, g_writes[w].val);
      w++;
    }
    if (run <= 0 && done >= fr->ncycles && w >= wend) break;
    seg++;
  }
  printf("debug_frame: per-sample lockstep found NO divergence (chunking bug: retry chunked)\n");
  /* If per-sample agrees, locate the first real write-delimited block and the
   * shortest one-shot prefix which diverges. This preserves the production
   * chunking that a dspb_run(..., 1) lockstep necessarily destroys. */
  memcpy(&R, &fr->snap, sizeof(Dsp)); memcpy(rr, fr->ramSnap, 0x10000); R.apu_ram = rr;
  memcpy(&B, &fr->snap, sizeof(Dsp)); memcpy(rb, fr->ramSnap, 0x10000); B.apu_ram = rb;
  uint8_t *preRram = malloc(0x10000), *preBram = malloc(0x10000);
  uint8_t *tryRram = malloc(0x10000), *tryBram = malloc(0x10000);
  static Dsp preR, preB, tryR, tryB;
  done = 0; w = fr->wfirst; seg = 0;
  while (done < fr->ncycles || w < wend) {
    int32_t next = (w < wend) ? g_writes[w].cyc : fr->ncycles;
    if (next > fr->ncycles) next = fr->ncycles;
    int32_t run = next - done;
    if (run > 0) {
      memcpy(&preR, &R, sizeof(Dsp)); memcpy(preRram, rr, 0x10000); preR.apu_ram = preRram;
      memcpy(&preB, &B, sizeof(Dsp)); memcpy(preBram, rb, 0x10000); preB.apu_ram = preBram;
      for (int32_t k = 0; k < run; k++) dsp_cycle(&R);
      dsp_runBlock(&B, run);
      if (memcmp((uint8_t*)&R + cmpOff, (uint8_t*)&B + cmpOff, sizeof(Dsp) - cmpOff) ||
          memcmp(rr, rb, 0x10000)) {
        printf("first chunk divergence: segment %d abs cycle %d len %d",
               seg, (int)done, (int)run);
        if (w > fr->wfirst)
          printf(" after write %02x=%02x@%d", g_writes[w-1].adr,
                 g_writes[w-1].val, (int)g_writes[w-1].cyc);
        if (w < wend)
          printf(" before write %02x=%02x@%d", g_writes[w].adr,
                 g_writes[w].val, (int)g_writes[w].cyc);
        printf("\n");
        for (int32_t k = 1; k <= run; k++) {
          memcpy(&tryR, &preR, sizeof(Dsp)); memcpy(tryRram, preRram, 0x10000); tryR.apu_ram = tryRram;
          memcpy(&tryB, &preB, sizeof(Dsp)); memcpy(tryBram, preBram, 0x10000); tryB.apu_ram = tryBram;
          for (int32_t j = 0; j < k; j++) dsp_cycle(&tryR);
          dsp_runBlock(&tryB, k);
          if (memcmp((uint8_t*)&tryR + cmpOff, (uint8_t*)&tryB + cmpOff, sizeof(Dsp) - cmpOff) ||
              memcmp(tryRram, tryBram, 0x10000)) {
            printf("shortest divergent one-shot prefix: %d samples (abs cycle %d)\n",
                   (int)k, (int)(done + k - 1));
            printf("  pre echo: writes=%d base=%04x index=%u remain=%u delay=%u firstAdr=%04x\n",
                   preB.echoWrites, preB.echoBufferAdr, preB.echoBufferIndex,
                   preB.echoRemain, preB.echoDelay,
                   (uint16_t)(preB.echoBufferAdr + preB.echoBufferIndex * 4));
            printf("  end echo: ref index=%u remain=%u blk index=%u remain=%u\n",
                   tryR.echoBufferIndex, tryR.echoRemain,
                   tryB.echoBufferIndex, tryB.echoRemain);
            const uint8_t *a=(const uint8_t*)&tryR+cmpOff, *b=(const uint8_t*)&tryB+cmpOff;
            for (size_t o = 0; o < sizeof(Dsp)-cmpOff; o++)
              if (a[o]!=b[o]) printf("  struct+%zu: ref=%02x blk=%02x\n", o, a[o], b[o]);
            for (int ch = 0; ch < 8; ch++) {
              dump_ch("pre", &preB, ch);
              dump_ch("ref", &tryR, ch);
              dump_ch("blk", &tryB, ch);
            }
            break;
          }
        }
        break;
      }
      done = next;
    }
    while (w < wend && g_writes[w].cyc <= done) {
      dsp_write(&R, g_writes[w].adr, g_writes[w].val);
      dsp_write(&B, g_writes[w].adr, g_writes[w].val);
      w++;
    }
    if (run <= 0 && done >= fr->ncycles && w >= wend) break;
    seg++;
  }
  free(preRram); free(preBram); free(tryRram); free(tryBram);
  free(rr); free(rb);
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: mixer_ab <rom> [frames] [bench-reps]\n"); return 2; }
  int frames = argc > 2 ? atoi(argv[2]) : 1200;
  int reps   = argc > 3 ? atoi(argv[3]) : 3;
  if (frames > MAX_FRAMES) frames = MAX_FRAMES;

  FILE *f = fopen(argv[1], "rb");
  if (!f) { printf("no rom: %s\n", argv[1]); return 1; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  long hdr = (n % 1024 == 512) ? 512 : 0;
  fseek(f, hdr, SEEK_SET); n -= hdr;
  uint8_t *rom = malloc(n);
  if (fread(rom, 1, n, f) != (size_t)n) return 1;
  fclose(f);

  Snes *snes = snes_init(g_wram);
  g_the_snes = snes;
  if (!snes_loadRom(snes, rom, (int)n)) { printf("unsupported ROM\n"); return 1; }

  /* ---- capture phase ---- */
  for (int i = 0; i < frames; i++) {
    snes->input1->currentState = 0;
    FrameRec *fr = &g_frames[g_nframes];
    if (snes->apu) {
      fr->ramSnap = malloc(0x10000);
      memcpy(&fr->snap, snes->apu->dsp, sizeof(Dsp));
      memcpy(fr->ramSnap, snes->apu->ram, 0x10000);
      fr->wfirst = g_nwrites;
      g_cyc = 0;
      g_capturing = true;
    }
    PpuBeginDrawing(snes->ppu, (uint8_t *)(g_fb + 32), 320 * 2, 0);
    run_frame_events(snes);
    if (snes->apu) {
      while (snes->apu->dsp->sampleOffset < 534) apu_cycle(snes->apu);
      g_capturing = false;
      fr->ncycles = g_cyc;
      fr->wcount = g_nwrites - fr->wfirst;
      memcpy(fr->liveBuf, snes->apu->dsp->sampleBuffer, sizeof(fr->liveBuf));
      snes->apu->dsp->sampleOffset = 0;   /* survey drain pattern */
      g_nframes++;
    }
  }
  if (!g_nframes) { printf("no APU frames captured\n"); return 1; }
  long totalCycles = 0, totalWrites = 0;
  for (int i = 0; i < g_nframes; i++) { totalCycles += g_frames[i].ncycles; totalWrites += g_frames[i].wcount; }
  printf("captured %d frames, %ld dsp cycles, %ld reg writes (%.1f/frame)\n",
         g_nframes, totalCycles, totalWrites, (double)totalWrites / g_nframes);
  uint8_t seenPmon = 0, seenNoise = 0, seenEcho = 0, seenKon = 0, seenKof = 0;
  int maxLiveVoices = 0, echoWriteFrames = 0;
  for (int i = 0; i < g_nframes; i++) {
    int liveVoices = 0;
    for (int ch = 0; ch < 8; ch++) {
      if (g_frames[i].snap.channel[ch].pitchModulation) seenPmon |= 1 << ch;
      if (g_frames[i].snap.channel[ch].useNoise) seenNoise |= 1 << ch;
      if (g_frames[i].snap.channel[ch].echoEnable) seenEcho |= 1 << ch;
      if (g_frames[i].snap.channel[ch].adsrState != 4 || g_frames[i].snap.channel[ch].gain)
        liveVoices++;
    }
    if (liveVoices > maxLiveVoices) maxLiveVoices = liveVoices;
    if (g_frames[i].snap.echoWrites) echoWriteFrames++;
  }
  for (int i = 0; i < g_nwrites; i++) {
    if (g_writes[i].adr == 0x2d) seenPmon |= g_writes[i].val;
    if (g_writes[i].adr == 0x3d) seenNoise |= g_writes[i].val;
    if (g_writes[i].adr == 0x4d) seenEcho |= g_writes[i].val;
    if (g_writes[i].adr == 0x4c) seenKon |= g_writes[i].val;
    if (g_writes[i].adr == 0x5c) seenKof |= g_writes[i].val;
  }
  printf("features: PMON=%02x NON=%02x EON=%02x KON=%02x KOF=%02x maxLiveVoices=%d echoWriteFrames=%d\n",
         seenPmon, seenNoise, seenEcho, seenKon, seenKof, maxLiveVoices, echoWriteFrames);

  /* ---- gate: ref vs block on identical inputs ---- */
  static Dsp instR, instB;
  uint8_t *ramR = malloc(0x10000), *ramB = malloc(0x10000);
  int liveDiffFrames = 0;
  const size_t cmpOff = offsetof(Dsp, ram);   /* skip the apu_ram pointer */
#ifdef DSP_MIXER_DIAG
  dspb_diag_reset();
#endif
  for (int i = 0; i < g_nframes; i++) {
    replay(&g_frames[i], &instR, ramR, 0);
    replay(&g_frames[i], &instB, ramB, 1);
    if (memcmp((uint8_t *)&instR + cmpOff, (uint8_t *)&instB + cmpOff, sizeof(Dsp) - cmpOff)) {
      const uint8_t *a = (const uint8_t *)&instR + cmpOff, *b = (const uint8_t *)&instB + cmpOff;
      size_t o; for (o = 0; o < sizeof(Dsp) - cmpOff; o++) if (a[o] != b[o]) break;
      printf("GATE FAIL frame %d: Dsp state mismatch at struct offset %zu (+%zu): ref=%02x blk=%02x\n",
             i, cmpOff + o, o, a[o], b[o]);
      debug_frame(&g_frames[i]);
      return 1;
    }
    if (memcmp(ramR, ramB, 0x10000)) {
      int o; for (o = 0; o < 0x10000; o++) if (ramR[o] != ramB[o]) break;
      printf("GATE FAIL frame %d: ARAM mismatch at %04x: ref=%02x blk=%02x\n", i, o, ramR[o], ramB[o]);
      return 1;
    }
    if (memcmp(instR.sampleBuffer, g_frames[i].liveBuf, sizeof(instR.sampleBuffer)))
      liveDiffFrames++;   /* SPC700 wrote ARAM mid-frame -- info only */
  }
  printf("GATE PASS: %d frames bit-identical (Dsp state + 64K ARAM + 534 samples)\n", g_nframes);
#ifdef DSP_MIXER_DIAG
  printf("fallback: %llu/%llu samples (%.3f%%), %llu hazard chunks, %llu PMON+echo chunks\n",
         (unsigned long long)dspb_diag_ref_samples,
         (unsigned long long)(dspb_diag_block_samples + dspb_diag_ref_samples),
         100.0 * dspb_diag_ref_samples /
           (dspb_diag_block_samples + dspb_diag_ref_samples),
         (unsigned long long)dspb_diag_hazard_chunks,
         (unsigned long long)dspb_diag_pmon_echo_chunks);
#endif
  printf("  (offline-ref vs live-ref differed on %d frames -- mid-frame SPC700 ARAM writes; not gated)\n",
         liveDiffFrames);

  /* ---- bench: replay all frames, both engines, reps times ----
   * MIXER_ENGINE=ref|blk benches only one engine (for perf stat isolation). */
  const char *engSel = getenv("MIXER_ENGINE");
  bool doRef = !engSel || !strcmp(engSel, "ref");
  bool doBlk = !engSel || !strcmp(engSel, "blk");
  double bestRef = 1e18, bestBlk = 1e18;
  for (int r = 0; r < reps; r++) {
    double t0 = now_ms();
    if (doRef) for (int i = 0; i < g_nframes; i++) replay(&g_frames[i], &instR, ramR, 0);
    double t1 = now_ms();
    if (doBlk) for (int i = 0; i < g_nframes; i++) replay(&g_frames[i], &instB, ramB, 1);
    double t2 = now_ms();
    if (t1 - t0 < bestRef) bestRef = t1 - t0;
    if (t2 - t1 < bestBlk) bestBlk = t2 - t1;
  }
  double nsRef = bestRef * 1e6 / totalCycles, nsBlk = bestBlk * 1e6 / totalCycles;
  printf("bench (%d reps, best): ref %.1f ms (%.1f ns/sample)  block %.1f ms (%.1f ns/sample)  ratio %.2fx\n",
         reps, bestRef, nsRef, bestBlk, nsBlk, nsRef / nsBlk);
  return 0;
}
