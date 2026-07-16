/* N-SPC sound-HLE wired into the LIVE emulator path.
 *
 * Until now the HLE (tools/nspc_hle) played from an ARAM snapshot AFTER
 * emulation. This wires it in while the game runs:
 *
 *   boot (LLE)          the real SPC700 runs the IPL handshake and the game
 *                       uploads its driver+data into ARAM — cheap and brief.
 *   detect              every 60 frames, scan ARAM with the survey signatures;
 *                       when the N-SPC driver + song table are recovered
 *                       (chOK>=6, known dialect) twice in a row -> swap.
 *   swap                adopt ARAM into a native SpcPlayer (nspc_hle state
 *                       hygiene), re-kick the last song the game commanded.
 *   after swap          apu_run() no longer runs SPC700 opcodes: it advances
 *                       the native player's 500 Hz tick + dsp_cycle at sample
 *                       rate, then mirrors the player's ports into
 *                       apu->outPorts, so the game's $2140-43 polls see the
 *                       native engine. Port writes land in input_ports[0]
 *                       (music protocol); ports 1-3 are instant-acked (foreign
 *                       SFX protocols are NOT implemented — the ack keeps
 *                       games moving, their sfx stay silent).
 *   fallback            if detection never succeeds, nothing changes: LLE runs
 *                       exactly as before (apu_run dispatches to the original).
 *
 * NOT bit-identical to LLE by design: the native driver acks faster than the
 * real one, so the 65816 sees different port-wait timing. Gates are scene
 * milestones + no-hang + audible audio, not state hashes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "src/snes/snes.h"
#include "src/snes/apu.h"
#include "src/snes/spc.h"
#include "src/snes/dsp.h"
#include "spc_player.h"
#include "nspc_config.h"
#include "snes_driver_sigs.h"
#include "wire.h"

/* nspc_variant.c */
void nspc_variant_std(void);
void nspc_variant_earlier(void);
void nspc_variant_gd3(int baseAddr, int tunLow, int tunCnt);

/* exported from the generated player copy (sed: static removed) */
void Spc_Loop_Part1(SpcPlayer *p);
void Spc_Loop_Part2(SpcPlayer *p, uint8_t ticks);

/* the original apu_run, renamed by build.sh in a copy of apu.c */
void apu_run_lle(Apu *apu, int cyclesToRun);

int        g_wire_on = 0;          /* 0 = LLE, 1 = native player active */
int        g_wire_enable = 1;      /* master switch (WIRE=0 env / rig -D) */
SpcPlayer *g_wire_p = NULL;
const char *g_wire_variant = "-";
static int g_frac = 0;             /* APU-cycle remainder (32 = one sample) */
static uint8_t g_ack[3];           /* instant-ack values for ports 1-3 */
static uint8_t g_last_p0 = 0;      /* last port-0 command seen during LLE */
static int g_ok_streak = 0;
static int g_native_ports = 0;     /* SM-exact layout: player IS the game's own
                                    * driver — feed all 4 ports, mirror all 4
                                    * echoes (SM handshakes on ports 1-3 and
                                    * black-screens on instant-acks). */

/* The generated player's SpcPlayer_Create allocates through this. On the
 * device the LLE Apu already owns 66 KB of the 120 KB AHB pool, so a second
 * 66 KB block cannot come from there — it lives in the overlay BSS (the
 * generic SNES overlay uses <30% of RAM_EMU). Host/rig: plain malloc. */
void *nspc_player_storage(void) {
#ifdef NSPC_WIRE_STATIC_PLAYER
  static SpcPlayer storage;
  return &storage;
#else
  return malloc(sizeof(SpcPlayer));
#endif
}

/* Between ROMs / on savestate load: back to LLE; detection re-runs and
 * re-swaps ~2 s in. (The static player storage is reused on the next swap.) */
void wire_reset(void) {
  g_wire_on = 0;
  g_wire_p = NULL;
  g_wire_variant = "-";
  g_frac = 0;
  g_ok_streak = 0;
  g_last_p0 = 0;
  g_native_ports = 0;
  g_ack[0] = g_ack[1] = g_ack[2] = 0;
}

/* ---- one 32 kHz sample step: exactly SpcPlayer_GenerateSamples' semantics -- */
static inline void wire_step_sample(SpcPlayer *p) {
  if (p->timer_cycles >= 64) {
    Spc_Loop_Part2(p, p->timer_cycles >> 6);
    Spc_Loop_Part1(p);
    p->timer_cycles &= 63;
  }
  p->timer_cycles++;
  dsp_cycle(p->dsp);
}

static inline void wire_mirror_ports(Apu *apu, SpcPlayer *p) {
  apu->outPorts[0] = p->port_to_snes[0];
  if (g_native_ports) {
    apu->outPorts[1] = p->port_to_snes[1];
    apu->outPorts[2] = p->port_to_snes[2];
    apu->outPorts[3] = p->port_to_snes[3];
  } else {
    apu->outPorts[1] = g_ack[0];
    apu->outPorts[2] = g_ack[1];
    apu->outPorts[3] = g_ack[2];
  }
}

/* Replaces apu.c's apu_run (renamed apu_run_lle in the copied file). */
void apu_run(Apu *apu, int cyclesToRun) {
  if (!g_wire_on) { apu_run_lle(apu, cyclesToRun); return; }
  SpcPlayer *p = g_wire_p;
  g_frac += cyclesToRun;
  while (g_frac >= 32) {           /* 1.024 MHz / 32 = 32 kHz sample clock */
    g_frac -= 32;
    wire_step_sample(p);
  }
  wire_mirror_ports(apu, p);
}

/* Harness RtlApuWrite routes here. adr = 0x2140..0x217f. */
void wire_apu_write(Snes *snes, uint32_t adr, uint8_t val) {
  snes_catchupApu(snes);
  int port = adr & 0x3;
  if (!g_wire_on) {
    snes->apu->inPorts[port] = val;
    /* remember the last real song command. Two filters: the handshake clears
     * the port to 0 after the ack (so "last value" recovers nothing), and
     * during the IPL upload port0 carries COUNTER bytes (0x8b garbage) — only
     * a write while the SPC700 executes the uploaded driver is a command. */
    if (port == 0 && val > 0 && val < 0xf0 && snes->apu->spc->pc < 0xffc0)
      g_last_p0 = val;
    return;
  }
  if (getenv("WIRE_TRACE")) {
    static int tr = 0;
    if (tr < 4000 && port == 0) { fprintf(stderr, "[wire] p0<=%02x\n", val); tr++; }
  }
  if (g_native_ports) {
    g_wire_p->input_ports[port] = val;   /* the player is this game's own driver */
  } else if (port == 0) {
    /* ALttP-style mailboxes write 00 when idle; SM's engine reads a 0 that
     * differs from the current song as a "stop" command. Idle-zero is not a
     * command — drop it (games stop via 0xf0 pause / 0xf1 fade instead). */
    if (val != 0)
      g_wire_p->input_ports[0] = val;    /* consumed by Music_HandleCmdFromSnes */
  } else {
    g_ack[port - 1] = val;               /* instant ack; SM sfx protocol not fed */
    g_wire_p->input_ports[port] = 0;
  }
  wire_mirror_ports(snes->apu, g_wire_p);
}

/* SM streams driver-level bank uploads through $80:8028 (block list in ROM:
 * {u16 numbytes, u16 target, bytes...}, 0-terminated — the exact format
 * SpcPlayer_Upload consumes). Against the real driver that routine port-
 * handshakes every byte; the native player cannot ack the stream, so the
 * game hangs. HLE the whole routine at entry: feed the blocks straight into
 * the player's ARAM and return as the homebrew's own RTS patch does
 * (sm_cpu_infra.c PatchBytes(0x808028, {0x60})). Called before every opcode by
 * the harness/porting loop; returns nonzero if the opcode was replaced. */
int wire_pre_opcode(Snes *snes) {
  if (!g_wire_on || !g_native_ports) return 0;
  Cpu *c = snes->cpu;
  /* Nothing above this line may be more than a compare: this runs once per
   * OPCODE. A getenv() trace probe sat here and cost SM +0.67M insn/frame —
   * two thirds of what the whole HLE saves. */
  if (c->k != 0x80 || c->pc != 0x8028) return 0;
  SpcPlayer *p = g_wire_p;
  /* 24-bit source address lives in the direct page, not registers — the
   * routine's own prologue is LDY $00 / LDA $02 / PLB (disassembled from the
   * ROM): dp+$00 = address word, dp+$02 = bank. */
  uint16_t dp = c->dp;
  uint16_t adr = (uint16_t)(snes->ram[(dp + 0) & 0x1fff] |
                            (snes->ram[(dp + 1) & 0x1fff] << 8));
  uint8_t bank = snes->ram[(dp + 2) & 0x1fff];
  if (getenv("WIRE_TRACE"))
    fprintf(stderr, "[wire] upload hook dp=%04x -> %02x:%04x\n", dp, bank, adr);
  dsp_write(p->dsp, 0x2c, 0);            /* EVOLL: echo off during upload */
  dsp_write(p->dsp, 0x3c, 0);            /* EVOLR */
  dsp_write(p->dsp, 0x5c, 0xff);         /* KOF: key off all voices */
  for (;;) {
    uint16_t numbytes = cart_read(snes->cart, bank, adr) |
                        (cart_read(snes->cart, bank, adr + 1) << 8);
    if (numbytes == 0) break;
    uint16_t target = cart_read(snes->cart, bank, adr + 2) |
                      (cart_read(snes->cart, bank, adr + 3) << 8);
    adr += 4;
    while (numbytes--) {
      p->ram[target++ & 0xffff] = cart_read(snes->cart, bank, adr++);
      if (adr == 0) bank++;              /* block lists can straddle banks */
    }
  }
  /* SpcPlayer_Upload's own tail: reset ports, arm the song at ram[$581e]. */
  p->port_to_snes[0] = 0;
  p->input_ports[0] = p->input_ports[1] = p->input_ports[2] = p->input_ports[3] = 255;
  p->music_ptr_toplevel = (uint16_t)(p->ram[0x581e] | (p->ram[0x581f] << 8));
  p->counter_sf0c = 2;
  p->key_OFF |= ~p->is_chan_on;
  wire_mirror_ports(snes->apu, p);
  /* Emulate the RTS the patch left behind: pull the 16-bit return address. */
  uint8_t lo = snes->ram[++c->sp & 0x1fff];   /* bank-0 stack = WRAM mirror */
  uint8_t hi = snes->ram[++c->sp & 0x1fff];
  c->pc = (uint16_t)(((hi << 8) | lo) + 1);
  return 6;                              /* RTS master-clock-ish cycle count */
}

/* Fill the player's DSP to a full 534-sample frame and fetch 16 kHz mono. */
void wire_frame_audio(int16_t *buf, int n) {
  SpcPlayer *p = g_wire_p;
  while (p->dsp->sampleOffset < 534) wire_step_sample(p);
  dsp_getSamples(p->dsp, buf, n, 1);
  if (getenv("WIRE_TRACE")) {
    static int fr = 0;
    if ((++fr % 200) == 0)
      fprintf(stderr, "[wire] f%d p0out=%02x mtl=%04x mvol=%04x tempo=%04x ff=%d ison=%02x ch0=%04x ch0vol=%d\n",
              fr, p->port_to_snes[0], p->music_ptr_toplevel, p->master_volume,
              p->tempo, p->fast_forward, p->is_chan_on,
              p->channel[0].pattern_order_ptr_for_chan, p->channel[0].final_volume);
  }
}

/* ================= detection (copied from tools/nspc_hle/nspc_poc.c) ======= */
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
static int sig_pos_zp(const uint8_t *ram, const char *name, int zp, int off1, int off2) {
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
  int gd3 = sig_pos_by_name(ram, "ptnIncSectionPtrGD3") >= 0;
  int smw = sig_pos_by_name(ram, "ptnJumpToVcmdSMW") >= 0;
  int zp = -1;
  { int ip = sig_pos_by_name(ram, "ptnIncSectionPtr");
    if (ip < 0) ip = sig_pos_by_name(ram, "ptnIncSectionPtrGD3");
    if (ip >= 0) zp = rd8(ram, ip + 3); }
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
  if (o->songList < 0 && want) {
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

/* ---- swap: adopt live ARAM into the native player (nspc_hle hygiene) ------ */
static void wire_swap(Snes *snes, const NspcParams *np, const uint8_t *aram) {
  if (!strcmp(np->variant, "GD3")) {
    int base_addr = 0, tunLow = 0, tunCnt = 0;
    int gp = sig_pos_by_name(aram, "ptnIncSectionPtrGD3");
    if (gp >= 0) { int zp = rd8(aram, gp + 16); base_addr = rd16(aram, zp); }
    int ip = sig_pos_by_name(aram, "ptnInstrVCmdGD3");
    if (ip >= 0) {
      int lo = rd16(aram, ip + 10), hi = rd16(aram, ip + 14);
      if (hi > lo && hi - lo <= 0x7f) { tunLow = lo; tunCnt = hi - lo; }
    }
    nspc_variant_gd3(base_addr, tunLow, tunCnt);
  } else if (!strcmp(np->variant, "SMW")) {
    nspc_variant_earlier();
  } else {
    nspc_variant_std();
  }
  /* The sig-extracted "songList" is the driver's COMPILED literal for
   * songList + (a-1)*2, i.e. the true base minus 2 (SM: 581e -> 5820,
   * ALttP: cffe -> d000 — both verified against the native decomp players).
   * The engine re-applies (a-1)*2, so feed it the true base or every song
   * command is off by one and song 1 reads an empty cell (instant stop —
   * ALttP's intro went silent exactly this way). std/YI share this codegen. */
  int isStdFamily = !strcmp(np->variant, "std") || !strcmp(np->variant, "YI");
  g_nspc_cfg.instrTable = np->instrTab >= 0 ? np->instrTab : 0x6c00;
  g_nspc_cfg.songList   = isStdFamily ? np->songList + 2 : np->songList;
  g_nspc_cfg.songCur    = np->songList - (isStdFamily ? 0 : 2);
  g_nspc_cfg.dirPage    = ((np->dir >= 0 ? np->dir : 0x6d00) >> 8) & 0xff;

  /* One player per session, reused across re-swaps (savestate load resets the
   * wire; a fresh SpcPlayer_Create would leak a second Dsp on the device). */
  static SpcPlayer *player_cache = NULL;
  SpcPlayer *p = player_cache;
  if (p == NULL) {
    p = player_cache = SpcPlayer_Create();
  } else {
    Dsp *d = p->dsp;                   /* separate allocation — keep it */
    memset(p, 0, sizeof(*p));
    p->dsp = d;                        /* d->apu_ram still points at p->ram */
  }
  memcpy(p->ram, aram, 0x10000);
  SpcPlayer_Initialize(p);
  memcpy(p->ram, aram, 0x10000);       /* re-seed what Vector_Reset memset */

  /* Initialize read SM's zero-page layout out of foreign ARAM — clear it
   * (the nspc_hle hard-won list; song start rebuilds via Music_ResetChan). */
  p->is_chan_on = 0; p->fast_forward = 0;
  p->key_ON = p->key_OFF = 0; p->cur_chan_bit = 0;
  p->vol_dirty = 0; p->sfx_timer_accum = 0; p->disable_sfx2 = 0;
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

  /* Resume the current song (music restarts from its top — a documented
   * behavior of the swap, not a bug). Best source: the REAL driver's own echo
   * on out-port 0 — N-SPC echoes the accepted song id there, and it is the
   * same engine we are swapping in. Fallback: last sniffed command write.
   * Last resort: the driver's own current-song cell in the adopted ARAM
   * (songList-2 by N-SPC engine layout — ALttP-style mailboxes idle port0
   * at 00, so both port sources come up empty there and only this works). */
  g_native_ports = (np->songList == 0x581e && np->instrTab == 0x6c00 &&
                    np->dir == 0x6d00);
  if (g_native_ports) {
    /* SM-exact layout: this ROM runs the very driver the player was decompiled
     * from. Start playback the way the engine's own SpcPlayer_Upload does —
     * the game keeps the current song pointer at ram[$581e], adopted with the
     * ARAM — and let every port speak the native protocol. */
    p->port_to_snes[0] = 0;
    p->input_ports[0] = p->input_ports[1] = p->input_ports[2] = p->input_ports[3] = 255;
    p->music_ptr_toplevel = (uint16_t)(p->ram[0x581e] | (p->ram[0x581f] << 8));
    p->counter_sf0c = 2;
    p->key_OFF |= ~p->is_chan_on;
  } else {
    uint8_t cur = snes->apu->outPorts[0];
    if (!(cur > 0 && cur < 0xf0)) cur = g_last_p0;
    if (!(cur > 0 && cur < 0xf0)) {
      uint8_t cell = aram[(g_nspc_cfg.songCur) & 0xffff];
      if (cell > 0 && cell < 0xf0) cur = cell;
    }
    p->port_to_snes[0] = 0;
    p->input_ports[0] = (cur > 0 && cur < 0xf0) ? cur : 255;
    p->input_ports[1] = p->input_ports[2] = p->input_ports[3] = 0;
  }

  g_wire_p = p;
  g_wire_variant = np->variant;
  g_ack[0] = g_ack[1] = g_ack[2] = 0;
  g_frac = 0;
  g_wire_on = 1;
  wire_mirror_ports(snes->apu, p);
}

/* Called by the harness once per frame during LLE. Returns 1 on swap. */
int wire_try_swap(Snes *snes, int frame) {
  if (g_wire_on || !g_wire_enable || !snes->apu) return 0;
  if (frame < 120 || (frame % 60) != 0) return 0;
  /* driver must actually be running (upload done, PC out of the IPL ROM) */
  if (snes->apu->spc->pc >= 0xffc0) { g_ok_streak = 0; return 0; }
  NspcParams np;
  if (!nspc_extract(snes->apu->ram, &np) || np.chOK < 6 ||
      !strcmp(np.variant, "?")) { g_ok_streak = 0; return 0; }
  /* The live PORT PROTOCOL is per-family, separate from the sequence dialect:
   * Konami (GD3) games block on stateful driver responses that instant-acks
   * cannot fake (TMNT hangs at boot). Only the SM/ALttP-lineage std protocol
   * is implemented — other variants stay LLE unless WIRE_ALL=1 (research). */
  if (strcmp(np.variant, "std") && strcmp(np.variant, "YI") && !getenv("WIRE_ALL"))
    { g_ok_streak = 0; return 0; }
  /* SM streams driver-level bank uploads through $80:8028-$8110; swapping
   * while the 65816 is inside that routine freezes the handshake it is mid-
   * way through (black screen). Defer — LLE finishes the upload, the next
   * 60-frame check lands outside. */
  if (np.songList == 0x581e && np.instrTab == 0x6c00 && np.dir == 0x6d00 &&
      snes->cpu->k == 0x80 && snes->cpu->pc >= 0x8028 && snes->cpu->pc < 0x8111)
    return 0;
  if (++g_ok_streak < 2) return 0;   /* stable across two checks 60 frames apart */
  wire_swap(snes, &np, snes->apu->ram);
  fprintf(stderr, "[wire] frame %d: swapped to native N-SPC (variant=%s song=%04x instr=%04x dir=%02x00 chOK=%d lastp0=%02x)\n",
          frame, np.variant, np.songList & 0xffff, g_nspc_cfg.instrTable & 0xffff,
          g_nspc_cfg.dirPage & 0xff, np.chOK, g_last_p0);
  return 1;
}
