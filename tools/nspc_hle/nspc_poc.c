/* N-SPC HLE proof-of-concept.
 *
 * Question: can Super Metroid's native N-SPC engine (spc_player.c) render a
 * DIFFERENT N-SPC game's music, given only that game's uploaded ARAM and the
 * three addresses our survey recovers (song list / instrument table / DIR)?
 *
 * Flow: boot the ROM in the emulated core so it uploads its driver+data to APU
 * RAM -> recover addresses (nspc_extract, copied from the survey) -> hand a fresh
 * SpcPlayer that ARAM and those addresses (via g_nspc_cfg) -> kick a song and run
 * the native engine, dumping a WAV and reporting peak amplitude. Non-silent output
 * = the generalized-spc_player plan is sound.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <libgen.h>

#include "src/snes/snes.h"
#include "src/snes/cart.h"
#include "src/snes/ppu.h"
#include "src/snes/apu.h"
#include "src/snes/cpu.h"
#include "src/snes/dma.h"
#include "src/snes/input.h"
#include "src/snes/dsp.h"
#include "spc_player.h"
#include "nspc_config.h"
#include "snes_driver_sigs.h"

/* nspc_variant.c owns g_nspc_cfg and the dialect tables */
void nspc_variant_std(void);
void nspc_variant_earlier(void);
void nspc_variant_gd3(int baseAddr, int tunLow, int tunCnt);

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
void RtlApuWrite(uint32_t adr, uint8_t val) {
  snes_catchupApu(g_the_snes);
  g_the_snes->apu->inPorts[adr & 0x3] = val;
}
static uint8_t  g_wram[0x20000];
static uint16_t g_fb[320 * 240];
static const double apuCyclesPerMaster = (32040 * 32) / (1364 * 262 * 60.0);

/* ---- boot/event loop (copied from snes_survey.c) --------------------------- */
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
      int cycles = cpu_runOpcode(snes->cpu);
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
        int cycles = cpu_runOpcode(snes->cpu);
        snes->cpuCyclesLeft += (cycles - snes->cpuMemOps) * 6;
      }
      snes->cpuCyclesLeft -= 2;
    }
    if (snes->hPos == 0 && snes->vPos == 0) break;
    run_dots(snes, dots_to_next_event(snes));
  }
  snes_catchupApu(snes);
}

/* ---- signature scan + N-SPC parameter recovery (copied from snes_survey.c) -- */
typedef struct { int songList, instrTab, dir, chOK; const char *variant; } NspcParams;
static bool sig_matches_at(const uint8_t *ram, int pos, const DriverSig *s) {
  for (int i = 0; i < s->len; i++)
    if (s->mask[i] == 'x' && ram[pos + i] != s->bytes[i]) return false;
  return true;
}
static int sig_pos_by_name(const uint8_t *ram, const char *name) {
  for (int i = 0; i < SNES_DRIVER_SIG_COUNT; i++) {
    const DriverSig *s = &SNES_DRIVER_SIGS[i];
    if (strcmp(s->name, name)) continue;
    int last = 0x10000 - s->len;
    for (int pos = 0; pos <= last; pos++)
      if (sig_matches_at(ram, pos, s)) return pos;
  }
  return -1;
}

/* The InitSectionPtr patterns wildcard the section-pointer zero-page byte that
 * VGMTrans PATCHES in before matching — unconstrained they hit the game's SFX
 * pointer tables too (SMW: 3 matches, first one wrong). Recreate the constraint:
 * the match must store to the ZP address the IncSectionPtr walk actually uses.
 * off2 < 0 = single-byte check. */
static int sig_pos_zp(const uint8_t *ram, const char *name, int zp,
                      int off1, int off2) {
  if (zp < 0) return sig_pos_by_name(ram, name);
  for (int i = 0; i < SNES_DRIVER_SIG_COUNT; i++) {
    const DriverSig *s = &SNES_DRIVER_SIGS[i];
    if (strcmp(s->name, name)) continue;
    int last = 0x10000 - s->len;
    for (int pos = 0; pos <= last; pos++) {
      if (!sig_matches_at(ram, pos, s)) continue;
      if (ram[(pos + off1) & 0xffff] != zp) continue;
      if (off2 >= 0 && ram[(pos + off2) & 0xffff] != ((zp + 1) & 0xff)) continue;
      return pos;
    }
  }
  return -1;
}
static int rd8 (const uint8_t *r, int a) { return r[a & 0xffff]; }
static int rd16(const uint8_t *r, int a) { return r[a & 0xffff] | (r[(a + 1) & 0xffff] << 8); }
static int nspc_score_songlist(const uint8_t *ram, int songList) {
  if (songList <= 0 || songList >= 0xfffe) return -1;
  int best = 0;
  for (int probe = 0; probe < 4; probe++) {
    int sec = rd16(ram, songList + probe * 2);
    if (sec < 0x100 || sec >= 0xfff0) continue;
    int t = sec, ok = 0;
    for (int ch = 0; ch < 8; ch++) { int cp = rd16(ram, t); t += 2; if (cp == 0 || (cp >= 0x100 && cp < 0xffff)) ok++; }
    if (ok > best) best = ok;
  }
  return best;
}
static int nspc_extract(const uint8_t *ram, NspcParams *o) {
  o->songList = o->instrTab = o->dir = -1; o->chOK = -1; o->variant = "?";

  /* Dialect FIRST, by the decisive engine-code signatures (how VGMTrans decides):
   * GD3 = Konami's IncSectionPtr fork; SMW = the 0xDA-base JumpToVcmd. The
   * song-list address then comes from that dialect's own init pattern,
   * CONSTRAINED to the section-pointer ZP the IncSectionPtr walk uses
   * (unconstrained, the init patterns also hit SFX pointer tables). */
  int gd3 = sig_pos_by_name(ram, "ptnIncSectionPtrGD3") >= 0;
  int smw = sig_pos_by_name(ram, "ptnJumpToVcmdSMW") >= 0;
  int zp = -1;
  { int ip = sig_pos_by_name(ram, "ptnIncSectionPtr");
    if (ip < 0) ip = sig_pos_by_name(ram, "ptnIncSectionPtrGD3");
    if (ip >= 0) zp = rd8(ram, ip + 3); }
  /* zpOff1/zpOff2: where the init code stores the table walker's ZP address */
  const struct { const char *name, *label; int off, zp1, zp2; } SL[] = {
    { "makeInitSectionPtrGD3Pattern","GD3", 8, 11, -1 },
    { "makeInitSectionPtrYIPattern", "YI", 12, 15, -1 },
    { "makeInitSectionPtrSMWPattern","SMW", 3,  6, 11 },
    { "makeInitSectionPtrPattern",   "std", 5,  8, -1 },
  };
  const char *want = gd3 ? "GD3" : smw ? "SMW" : NULL;
  for (unsigned i = 0; i < sizeof(SL)/sizeof(SL[0]); i++) {
    int p;
    if (want && strcmp(SL[i].label, want)) continue;
    p = sig_pos_zp(ram, SL[i].name, zp, SL[i].zp1, SL[i].zp2);
    if (p < 0) continue;
    int cand = rd16(ram, p + SL[i].off), score = nspc_score_songlist(ram, cand);
    if (want) { o->chOK = score; o->songList = cand; o->variant = SL[i].label; break; }
    if (score > o->chOK) { o->chOK = score; o->songList = cand; o->variant = SL[i].label; }
  }
  if (o->songList < 0 && want) {   /* dialect known but its init pattern absent */
    for (unsigned i = 0; i < sizeof(SL)/sizeof(SL[0]); i++) {
      int p = sig_pos_zp(ram, SL[i].name, zp, SL[i].zp1, SL[i].zp2);
      if (p < 0) continue;
      int cand = rd16(ram, p + SL[i].off), score = nspc_score_songlist(ram, cand);
      if (score > o->chOK) { o->chOK = score; o->songList = cand; }
    }
    o->variant = want;
  }
  if (o->songList < 0) return 0;
  const struct { const char *name; int lo, hi; } IT[] = {
    { "ptnLoadInstrTableAddress", 7, 10 }, { "ptnLoadInstrTableAddressSMW", 3, 6 },
    { "ptnLoadInstrTableAddressCTOW", 7, 10 }, { "ptnLoadInstrTableAddressSOS", 1, 4 },
  };
  for (unsigned i = 0; i < sizeof(IT)/sizeof(IT[0]); i++) {
    int p = sig_pos_by_name(ram, IT[i].name);
    if (p >= 0) { o->instrTab = rd8(ram, p + IT[i].lo) | (rd8(ram, p + IT[i].hi) << 8); break; }
  }
  const struct { const char *name; int off; } DR[] = {
    { "ptnSetDIR", 4 }, { "ptnSetDIRYI", 1 }, { "ptnSetDIRSMW", 9 }, { "ptnSetDIRCTOW", 3 },
  };
  for (unsigned i = 0; i < sizeof(DR)/sizeof(DR[0]); i++) {
    int p = sig_pos_by_name(ram, DR[i].name);
    if (p >= 0) { o->dir = rd8(ram, p + DR[i].off) << 8; break; }
  }
  return 1;
}

/* ---- WAV ------------------------------------------------------------------- */
static int16_t g_wav[16000 * 20]; static int g_wavlen;
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
  if (argc < 2) { fprintf(stderr, "usage: nspc_poc <rom> [songid] [frames]\n"); return 2; }
  int songid = argc > 2 ? atoi(argv[2]) : 1;
  int frames = argc > 3 ? atoi(argv[3]) : 600;
  char pc[1024]; snprintf(pc, sizeof(pc), "%s", argv[1]); const char *base = basename(pc);

  /* load ROM */
  FILE *f = fopen(argv[1], "rb"); if (!f) { printf("no rom\n"); return 1; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  long hdr = (n % 1024 == 512) ? 512 : 0; fseek(f, hdr, SEEK_SET); n -= hdr;
  uint8_t *rom = malloc(n); if (fread(rom, 1, n, f) != (size_t)n) return 1; fclose(f);

  /* boot so the game uploads its N-SPC driver+data to APU RAM */
  Snes *snes = snes_init(g_wram); g_the_snes = snes;
  if (!snes_loadRom(snes, rom, (int)n)) { printf("%s: LOAD_FAIL\n", base); return 1; }
  for (int i = 0; i < frames; i++) {
    snes->input1->currentState = 0;
    PpuBeginDrawing(snes->ppu, (uint8_t *)(g_fb + 32), 320 * 2, 0);
    run_frame_events(snes);
    if (snes->apu) { while (snes->apu->dsp->sampleOffset < 534) apu_cycle(snes->apu); snes->apu->dsp->sampleOffset = 0; }
  }
  if (!snes->apu) { printf("%s: NO_APU\n", base); return 1; }

  NspcParams np;
  const uint8_t *aram = snes->apu->ram;
  if (!nspc_extract(aram, &np) || np.songList < 0) { printf("%s: not N-SPC / no song list\n", base); return 1; }
  printf("%s  variant=%s song=%04x instr=%04x dir=%04x chOK=%d\n",
         base, np.variant, np.songList & 0xffff, np.instrTab & 0xffff, np.dir & 0xffff, np.chOK);
  if (np.instrTab < 0) np.instrTab = 0x6c00;   /* fallback: many N-SPC use SM's page */
  if (np.dir < 0) np.dir = 0x6d00;

  /* select the sequence dialect (+ GD3 base address / tuning tables) */
  if (!strcmp(np.variant, "GD3")) {
    int base_addr = 0, tunLow = 0, tunCnt = 0;
    int gp = sig_pos_by_name(aram, "ptnIncSectionPtrGD3");
    if (gp >= 0) { int zp = rd8(aram, gp + 16); base_addr = rd16(aram, zp); }
    int ip = sig_pos_by_name(aram, "ptnInstrVCmdGD3");
    if (ip >= 0) {
      int lo = rd16(aram, ip + 10), hi = rd16(aram, ip + 14);
      if (hi > lo && hi - lo <= 0x7f) { tunLow = lo; tunCnt = hi - lo; }
    }
    printf("   gd3: base=%04x tuning=%04x+%d\n", base_addr, tunLow, tunCnt);
    nspc_variant_gd3(base_addr, tunLow, tunCnt);
  } else if (!strcmp(np.variant, "SMW")) {
    nspc_variant_earlier();
  } else {
    nspc_variant_std();
  }

  /* hand the native engine this game's ARAM + recovered addresses */
  g_nspc_cfg.instrTable = np.instrTab;
  g_nspc_cfg.songList   = np.songList;
  g_nspc_cfg.songCur    = np.songList - 2;
  g_nspc_cfg.dirPage    = (np.dir >> 8) & 0xff;

  /* songid > 0: play that song. songid == 0: scan songs 1..8, report each. */
  int s_lo = songid ? songid : 1, s_hi = songid ? songid : 8;
  int best_peak = 0, best_song = 0;
  const char *wp = getenv("NSPC_WAV");

  for (int sid = s_lo; sid <= s_hi; sid++) {
    SpcPlayer *p = SpcPlayer_Create();
    static DspRegWriteHistory dbg_hist;
    if (getenv("NSPC_DBG")) { dbg_hist.count = 0; p->reg_write_history = &dbg_hist; }
    memcpy(p->ram, aram, 0x10000);
    SpcPlayer_Initialize(p);           /* Vector_Reset: DSP reset, DIR, tempo, echo */
    memcpy(p->ram, aram, 0x10000);     /* re-seed data Vector_Reset's memsets clobbered */

    /* Initialize ran SpcPlayer_CopyVariablesFromRam, which reads SM's zero-page
     * LAYOUT out of this foreign game's ARAM — pure garbage. Whether music plays
     * then depends on what the game left at SM's addresses at snapshot time
     * (SMW: boot=60 played, boot=300 loaded is_chan_on garbage that gated every
     * DSP write). Clear the engine state; the song-start path (Music_ResetChan)
     * rebuilds the rest. */
    p->is_chan_on = 0;
    p->fast_forward = 0;
    p->key_ON = p->key_OFF = 0;
    p->cur_chan_bit = 0;
    p->vol_dirty = 0;
    p->sfx_timer_accum = 0;
    p->disable_sfx2 = 0;
    memset(&p->sfx1, 0, sizeof(p->sfx1));
    memset(&p->sfx2, 0, sizeof(p->sfx2));
    memset(&p->sfx3, 0, sizeof(p->sfx3));
    memset(p->sfx_chans_1, 0, sizeof(p->sfx_chans_1));
    memset(p->sfx_chans_2, 0, sizeof(p->sfx_chans_2));
    memset(p->sfx_chans_3, 0, sizeof(p->sfx_chans_3));
    for (int ch = 0; ch < 8; ch++) {
      p->channel[ch].pattern_order_ptr_for_chan = 0;
      p->channel[ch].cutk = 0;
      p->channel[ch].index = ch;
    }

    /* kick the song: standard N-SPC start protocol via port 0 */
    p->port_to_snes[0] = 0;
    p->input_ports[0] = sid;
    p->input_ports[1] = p->input_ports[2] = p->input_ports[3] = 255;

    int16_t buf[16000 / 60];
    int peak = 0;
    long energy_n = 0; double energy = 0;
    g_wavlen = 0;
    for (int i = 0; i < frames; i++) {
      SpcPlayer_GenerateSamples(p);                 /* fills p->dsp with 534 samples */
      dsp_getSamples(p->dsp, buf, 16000 / 60, 1);   /* -> 16kHz mono, resets sampleOffset */
      for (int s = 0; s < 16000 / 60; s++) {
        int a = buf[s]; if (a < 0) a = -a; if (a > peak) peak = a;
        energy += (double)buf[s] * buf[s]; energy_n++;
        if (g_wavlen < (int)(sizeof(g_wav) / 2)) g_wav[g_wavlen++] = buf[s];
      }
    }
    if (getenv("NSPC_DBG")) {
      fprintf(stderr, "[dbg] song %d: %u dsp writes:", sid, dbg_hist.count);
      for (unsigned k = 0; k < dbg_hist.count && k < 96; k++)
        fprintf(stderr, " %02x=%02x", dbg_hist.addr[k], dbg_hist.val[k]);
      fprintf(stderr, "\n[dbg] port0=%d ctr=%d mtl=%04x ch0ptr=%04x ch0instr=%d tempo=%04x ff=%d\n",
              p->port_to_snes[0], p->counter_sf0c, p->music_ptr_toplevel,
              p->channel[0].pattern_order_ptr_for_chan, p->channel[0].instrument_id,
              p->tempo, p->fast_forward);
    }
    int rms = energy_n ? (int)__builtin_sqrt(energy / energy_n) : 0;
    printf("   song %d: peak=%d rms=%d  %s\n", sid, peak, rms,
           peak > 300 ? "NON-SILENT ***" : "silent");
    if (peak > best_peak) {
      best_peak = peak; best_song = sid;
      if (wp) wav_write(wp);
    }
    free(p);
  }
  if (!wp) wav_write("/tmp/nspc_poc.wav");
  printf("   best: song %d peak=%d\n", best_song, best_peak);
  return 0;
}
