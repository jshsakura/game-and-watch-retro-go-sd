/* Host harness for the live-wired N-SPC HLE (wire.c).
 *
 * Runs a ROM with Start-tap input. WIRE=0 -> pure LLE reference; WIRE=1
 * (default) -> boot LLE, auto-swap to the native player when detected.
 * Prints per-window fb hash / lit / 65816 opcode count (scene milestones +
 * the TMNT spin-collapse question), ms/frame, and dumps the HLE audio WAV.
 *
 * NOT a state-hash gate: post-swap timing legitimately diverges from LLE
 * (the native driver acks faster). Gates: scenes reached, no hang, audio.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <libgen.h>

#include "src/snes/snes.h"
#include "src/snes/cart.h"
#include "src/snes/ppu.h"
#include "src/snes/apu.h"
#include "src/snes/cpu.h"
#include "src/snes/dma.h"
#include "src/snes/input.h"
#include "src/snes/dsp.h"
#include "wire.h"

bool snes_loadRom(Snes* snes, const uint8_t* data, int length);

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
/* ---- $2140-$2143 port trace --------------------------------------------
 * PORT_TRACE=<file> logs the 65816<->APU mailbox with a frame/scanline
 * timestamp, in BOTH modes, so an LLE run and a wire run can be diffed to the
 * first divergence.
 *
 * Why this and not a static diff of the four decompiled players: none of their
 * *Upload() functions reproduces the real mailbox at all -- they memcpy an
 * asset block into ARAM and tidy the native state. The actual
 * 0xff -> aa/bb -> cc -> counter wire lives outside them, so the handshake a
 * game really performs is only observable by running it.
 *
 * Writes are hooked here (RtlApuWrite is ours, so no submodule is touched).
 * Reads are NOT hooked -- snes_readBBus lives in the submodule -- so the
 * driver's replies are captured by polling apu->outPorts for changes, which
 * sees every value the 65816 could have read as long as the poll is finer
 * than the driver's update rate. wire.c mirrors the native player's
 * port_to_snes into outPorts, so the same poll works with the wire on. */
static FILE *g_pt;
static int   g_pt_frame;
static uint32_t g_pt_last_out = 0xffffffffu;

static void pt_open(void) {
  const char *path = getenv("PORT_TRACE");
  if (path && *path) g_pt = fopen(path, "w");
}

static void pt_poll(Snes *snes) {
  if (!g_pt || !snes->apu) return;
  const uint8_t *o = snes->apu->outPorts;
  uint32_t cur = (uint32_t)o[0] | (o[1] << 8) | (o[2] << 16) | ((uint32_t)o[3] << 24);
  if (cur == g_pt_last_out) return;
  g_pt_last_out = cur;
  fprintf(g_pt, "%5d %4d %4d OUT %02x %02x %02x %02x\n",
          g_pt_frame, snes->vPos, snes->hPos, o[0], o[1], o[2], o[3]);
}

void RtlApuWrite(uint32_t adr, uint8_t val) {
  if (g_pt && (adr & 0x7c) == 0x40) {
    const uint8_t *o = g_the_snes->apu ? g_the_snes->apu->outPorts : NULL;
    fprintf(g_pt, "%5d %4d %4d W%d  %02x            pc=%04x out=%02x%02x%02x%02x\n",
            g_pt_frame, g_the_snes->vPos, g_the_snes->hPos, (int)(adr & 3), val,
            g_the_snes->apu ? g_the_snes->apu->spc->pc : 0,
            o ? o[0] : 0, o ? o[1] : 0, o ? o[2] : 0, o ? o[3] : 0);
  }
  wire_apu_write(g_the_snes, adr, val);   /* LLE passthrough inside */
  if (g_pt) pt_poll(g_the_snes);
}

static uint8_t  g_wram[0x20000];
static uint16_t g_fb[320 * 240];
static uint64_t g_opcodes;               /* 65816 opcodes this frame */
static const double apuCyclesPerMaster = (32040 * 32) / (1364 * 262 * 60.0);

static int dots_to_next_event(Snes *snes) {
  int h = snes->hPos;
  if (h == 0 || h == 512 || h == 1024) return 0;
  if (snes->hIrqEnabled && h == snes->hTimer * 4) return 0;
  int next = 1362;
  if (h < 512) next = 512; else if (h < 1024) next = 1024;
  if (snes->hIrqEnabled) { int t = snes->hTimer * 4; if (t > h && t < next) next = t; }
  return next - h;
}
static void apply_irq_match(Snes *snes) {
  if (!(snes->hIrqEnabled || snes->vIrqEnabled)) return;
  if (snes->vIrqEnabled && snes->vPos != snes->vTimer) return;
  if (snes->hIrqEnabled && snes->hPos != snes->hTimer * 4) return;
  snes->inIrq = true; snes->cpu->irqWanted = true;
}
static void run_dots(Snes *snes, int dots) {
  while (dots > 0) {
    if (snes->dma->dmaBusy || snes->dma->hdmaTimer > 0) {
      dma_cycle(snes->dma); snes->apuCatchupCycles += apuCyclesPerMaster * 2.0;
      snes->hPos += 2; dots -= 2; continue;
    }
    bool started_dma = false;
    if (snes->cpuCyclesLeft == 0) {
      apply_irq_match(snes); snes->cpuMemOps = 0;
      int cycles = cpu_runOpcode(snes->cpu); g_opcodes++;
      snes->cpuCyclesLeft += (cycles - snes->cpuMemOps) * 6;
      started_dma = snes->dma->dmaBusy || snes->dma->hdmaTimer > 0;
    }
    int step;
    if (snes->cpuCyclesLeft >= 2 && !started_dma) {
      step = snes->cpuCyclesLeft; if (step > dots) step = dots; step &= ~1;
      snes->cpuCyclesLeft -= (uint8_t)step;
    } else { step = 2; snes->cpuCyclesLeft -= 2; }
    snes->apuCatchupCycles += apuCyclesPerMaster * step;
    snes->hPos += step; dots -= step;
  }
}
static void run_frame_events(Snes *snes) {
  for (;;) {
    snes->apuCatchupCycles += apuCyclesPerMaster * 2.0;
    snes_handle_pos_stuff(snes);
    if (dma_cycle(snes->dma)) {} else {
      if (snes->cpuCyclesLeft == 0) {
        snes->cpuMemOps = 0;
        int cycles = cpu_runOpcode(snes->cpu); g_opcodes++;
        snes->cpuCyclesLeft += (cycles - snes->cpuMemOps) * 6;
      }
      snes->cpuCyclesLeft -= 2;
    }
    if (snes->hPos == 0 && snes->vPos == 0) break;
    pt_poll(snes);
    run_dots(snes, dots_to_next_event(snes));
  }
  snes_catchupApu(snes);
  pt_poll(snes);
}

static uint64_t fnv1a(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data; uint64_t h = 1469598103934665603ULL;
  while (len--) { h ^= *p++; h *= 1099511628211ULL; }
  return h;
}

/* ---- WAV ------------------------------------------------------------------ */
static int16_t g_wav[16000 * 30]; static int g_wavlen;
static void wav_write(const char *path) {
  FILE *f = fopen(path, "wb"); if (!f) return;
  int rate = 16000, data = g_wavlen * 2;
  #define W16(v) do{uint16_t x=(v);fwrite(&x,2,1,f);}while(0)
  #define W32(v) do{uint32_t x=(v);fwrite(&x,4,1,f);}while(0)
  fwrite("RIFF",1,4,f); W32(36+data); fwrite("WAVE",1,4,f);
  fwrite("fmt ",1,4,f); W32(16); W16(1); W16(1); W32(rate); W32(rate*2); W16(2); W16(16);
  fwrite("data",1,4,f); W32(data); fwrite(g_wav,1,data,f); fclose(f);
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: host_main <rom> [frames=1500]\n"); return 2; }
  int frames = argc > 2 ? atoi(argv[2]) : 1500;
  const char *we = getenv("WIRE");
  if (we && atoi(we) == 0) g_wire_enable = 0;
  char pc[1024]; snprintf(pc, sizeof(pc), "%s", argv[1]);

  FILE *f = fopen(argv[1], "rb"); if (!f) { printf("no rom\n"); return 1; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  long hdr = (n % 1024 == 512) ? 512 : 0; fseek(f, hdr, SEEK_SET); n -= hdr;
  uint8_t *rom = malloc(n); if (fread(rom, 1, n, f) != (size_t)n) return 1; fclose(f);

  pt_open();
  Snes *snes = snes_init(g_wram); g_the_snes = snes;
  if (!snes_loadRom(snes, rom, (int)n)) { printf("LOAD_FAIL\n"); return 1; }

  int16_t abuf[16000 / 60];
  int peak = 0; double energy = 0; long energy_n = 0;
  uint64_t win_ops = 0;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  for (int i = 0; i < frames; i++) {
    g_pt_frame = i;
    snes->input1->currentState = (i >= 40 && (i % 24) < 6) ? 0x0008 : 0;
    PpuBeginDrawing(snes->ppu, (uint8_t *)(g_fb + 32), 320 * 2, 0);
    g_opcodes = 0;
    run_frame_events(snes);
    win_ops += g_opcodes;

    wire_try_swap(snes, i);

    if (g_wire_on) {
      wire_frame_audio(abuf, 16000 / 60);
    } else if (snes->apu) {
      while (snes->apu->dsp->sampleOffset < 534) apu_cycle(snes->apu);
      dsp_getSamples(snes->apu->dsp, abuf, 16000 / 60, 1);
    }
    static int wpeak; static double wenergy; static long wn;
    for (int s = 0; s < 16000 / 60; s++) {
      int a = abuf[s]; if (a < 0) a = -a; if (a > peak) peak = a;
      if (a > wpeak) wpeak = a;
      energy += (double)abuf[s] * abuf[s]; energy_n++;
      wenergy += (double)abuf[s] * abuf[s]; wn++;
      if (g_wavlen < (int)(sizeof(g_wav) / 2)) g_wav[g_wavlen++] = abuf[s];
    }

    if ((i + 1) % 200 == 0) {
      int lit = 0; for (int q = 0; q < 320 * 240; q++) if (g_fb[q]) lit++;
      int wrms = wn ? (int)__builtin_sqrt(wenergy / wn) : 0;
      printf("w%05d fb=%08lx lit=%d ops/frame=%lu apeak=%d arms=%d mode=%s\n",
             i + 1, (unsigned long)(uint32_t)fnv1a(g_fb, sizeof(g_fb)), lit,
             (unsigned long)(win_ops / 200), wpeak, wrms,
             g_wire_on ? g_wire_variant : "LLE");
      win_ops = 0; wpeak = 0; wenergy = 0; wn = 0;
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / frames;
  int rms = energy_n ? (int)__builtin_sqrt(energy / energy_n) : 0;
  printf("done %d frames  %.3f ms/frame  mode=%s  audio peak=%d rms=%d\n",
         frames, ms, g_wire_on ? g_wire_variant : "LLE", peak, rms);
  { const char *wp = getenv("WIRE_WAV"); if (wp) wav_write(wp); }
  return 0;
}
