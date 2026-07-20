/* N-SPC sound-HLE wired into the LIVE emulator path (firmware product).
 *
 * This is the device-ready generalization of Super Metroid's native sound
 * engine to the ~71% of SNES games that use the shared N-SPC driver.  The
 * detection is ARAM-signature-based (VGMTrans's 138 patterns), NOT full-ROM
 * CRC — so it works on any ROM that uploads the same engine, including hacks
 * and translations.  The gate is: detect a known N-SPC dialect (std / YI)
 * with chOK>=6 twice, 60 frames apart, after the driver upload completes.
 * Everything else stays LLE forever (fail-safe).
 *
 * Differences from the host proof-of-concept (tools/nspc_audio_wire/wire.c):
 *   - Zero-copy: SpcPlayer_CreateWithState(aram, dsp) reuses the live APU's
 *     64 KB ARAM and DSP object; no second allocation, no memcpy.
 *   - No SpcPlayer_Initialize: Vector_Reset_Spc memsets ARAM regions that
 *     belong to the foreign game's data layout; the native C state is set
 *     up manually instead (the song command restarts playback cleanly).
 *   - No getenv() / host trace fprintf: firmware uses hardcoded constants
 *     (min_frame=120, std/YI dialect only).
 *   - Savestate: prepare_save is a no-op (CopyVariablesToRam would corrupt
 *     foreign ARAM); restore_after_load re-adopts immediately using the
 *     preserved g_nspc_cfg (music restarts, matching the documented swap
 *     behavior).
 *
 * Lane: audio HLE only.  Codex owns ppu.c; no overlap.
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
#include "spc_player_gen.h"         /* generated zero-copy struct */
#include "nspc_config.h"            /* g_nspc_cfg, NSPC_* macros */
#include "snes_driver_sigs.h"       /* VGMTrans ARAM signatures */
#include "wire.h"

/* ---- nspc_variant.c (dialect tables) ---- */
void nspc_variant_std(void);
void nspc_variant_earlier(void);
void nspc_variant_gd3(int baseAddr, int tunLow, int tunCnt);

/* ---- exported from the generated player copy (static removed by generator) ---- */
void Spc_Loop_Part1(SpcPlayer *p);
void Spc_Loop_Part2(SpcPlayer *p, uint8_t ticks);
void Music_HandleCmdFromSnes(SpcPlayer *p);   /* exported for wire_swap()'s bootstrap */

/* ---- zero-copy player constructor (generated, replaces SpcPlayer_Create) ---- */
SpcPlayer *SpcPlayer_CreateWithState(uint8 *ram, Dsp *dsp);

/* ---- original apu_run, renamed in the generated copy of apu.c ---- */
void apu_run_lle(Apu *apu, int cyclesToRun);

/* ============================ state ====================================== */
int        g_wire_on = 0;          /* 0 = LLE, 1 = native player active */
int        g_wire_enable = 1;      /* master switch; detection active by default */
const char *g_wire_variant = "-";

static SpcPlayer *g_wire_p = NULL;
static int g_frac = 0;             /* APU-cycle remainder (32 = one sample) */
static uint8_t g_ack[3];           /* instant-ack values for ports 1-3 */
static uint8_t g_last_p0 = 0;      /* last port-0 command seen during LLE */
static int g_ok_streak = 0;
static int g_was_hle = 0;          /* 1 if HLE was active (for savestate) */
static int g_frame = 0;
/* 1 after a load whose save was taken while HLE was active: wire_try_swap()
 * should re-adopt as soon as outPorts[0] settles, skipping the slow "two
 * checks 60 frames apart" driver re-confirmation below (the driver was
 * already confirmed valid before the save -- only the NEW stability defense
 * needs to clear). See wire_restore_after_load(). */
static int g_load_pending_resume = 0;

/* Live outPorts[0] stability tracking (see wire_try_swap()'s gate below).
 * Sampled once per video frame regardless of the coarse 60-frame detection
 * cadence, so it reflects real elapsed-frame history by the time detection
 * is ready to fire. */
static uint8_t g_p0_last = 0;
static int g_p0_stable = 0;
/* wire_swap() must never adopt a live outPorts[0] echo while the real N-SPC
 * driver is still mid-handshake -- confirmed on Super Metroid (frames
 * 340-420 read-hook comparison): outPorts[0] can sit on a transient byte
 * the real SPC700 clears within a few ticks, but our snapshot-based
 * bootstrap freezes it as if settled, so the CPU's poll for the next
 * transition never resolves under HLE. This is a general timing race in
 * the swap mechanism (any game's swap frame can land mid-handshake), not
 * a Super Metroid-specific defect -- Zelda's swap merely happened to land
 * on an already-settled byte. Requiring N consecutive unchanged frames
 * before swapping is a game-agnostic gate: no hardcoded "settled value"
 * list, because the settled value differs per game and per song. */
#define NSPC_SWAP_STABLE_FRAMES 30

/* ===================== one 32 kHz sample step ============================ */
/* Exactly SpcPlayer_GenerateSamples' semantics: advance the 500 Hz N-SPC
 * driver tick (Spc_Loop_Part2 + Part1) when timer_cycles wraps, then one
 * dsp_cycle to produce one sample pair. */
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
  apu->outPorts[1] = g_ack[0];
  apu->outPorts[2] = g_ack[1];
  apu->outPorts[3] = g_ack[2];
}

/* ===================== apu_run override ================================== */
/* Replaces apu.c's apu_run (renamed apu_run_lle in the generated copy).
 * When HLE is active, we advance the native player at the 32 kHz sample
 * clock instead of interpreting SPC700 opcodes. */
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

/* ===================== port-write protocol =============================== */
/* The 65816 writes to $2140-43; we route through here.  During LLE, we sniff
 * port-0 song commands.  During HLE, port-0 drives the music engine and
 * ports 1-3 are instant-acked (foreign SFX protocols are NOT implemented —
 * the ack keeps games moving, their sfx stay silent). */
void wire_apu_write(Snes *snes, uint32_t adr, uint8_t val) {
  snes_catchupApu(snes);
  int port = adr & 0x3;
  g_frame++;  /* approximate frame counter for trace messages */
  if (!g_wire_on) {
    snes->apu->inPorts[port] = val;
    /* Remember the last real song command.  Two filters: the handshake
     * clears the port to 0 after the ack, and during IPL upload port0
     * carries counter bytes — only a write while the SPC700 executes the
     * uploaded driver is a command. */
    if (port == 0 && val > 0 && val < 0xf0 && snes->apu->spc->pc < 0xffc0)
      g_last_p0 = val;
    return;
  }
  if (port == 0) {
    /* ALttP-style mailboxes write 00 when idle; SM's engine reads a 0 that
     * differs from the current song as "stop".  Idle-zero is not a command
     * — drop it (games stop via 0xf0 pause / 0xf1 fade instead). */
    if (val != 0)
      g_wire_p->input_ports[0] = val;
  } else {
    g_ack[port - 1] = val;
    g_wire_p->input_ports[port] = 0;
  }
  wire_mirror_ports(snes->apu, g_wire_p);
}

/* ===================== frame audio fill ================================== */
/* Top the player's DSP up to a full 534-sample frame and fetch 16 kHz mono. */
void wire_frame_audio(int16_t *buf, int n) {
  SpcPlayer *p = g_wire_p;
  while (p->dsp->sampleOffset < 534) wire_step_sample(p);
  dsp_getSamples(p->dsp, buf, n, 1);
}

/* ===================== detection (from tools/nspc_hle/nspc_poc.c) ======== */
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
    { "ptnLoadInstrTableAddress", 7, 10 }, { "ptnLoadInstrTableAddressSmW", 3, 6 },
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

/* ===================== swap: adopt live ARAM (zero-copy) ================== */
static void wire_swap(Snes *snes, const NspcParams *np, const uint8_t *aram) {
  /* Dialect setup — same as host wire.c */
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
  g_nspc_cfg.instrTable = np->instrTab >= 0 ? np->instrTab : 0x6c00;
  g_nspc_cfg.songList   = np->songList;
  g_nspc_cfg.songCur    = np->songList - 2;
  g_nspc_cfg.dirPage    = ((np->dir >= 0 ? np->dir : 0x6d00) >> 8) & 0xff;

  /* Zero-copy adoption: reuse the live APU ARAM and DSP.  NO memcpy, NO
   * SpcPlayer_Initialize — Vector_Reset_Spc memsets ARAM regions that
   * belong to the foreign game's data, and dsp_reset would wipe the live
   * DSP.  The native C struct starts zeroed (CreateWithState memsets it),
   * then we set the fields the song-start needs. */
  SpcPlayer *p = SpcPlayer_CreateWithState((uint8 *)aram, snes->apu->dsp);

  /* Clear native sequencer state (matching host wire.c post-Initialize).
   * The song command restarts playback from ARAM, which is uncorrupted. */
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

  /* Resume the current song.  Best source: the real driver's echo on
   * out-port 0 (N-SPC echoes the accepted song id); fallback: last sniffed
   * command write.  Music restarts from its top — a documented behavior. */
  uint8_t cur = snes->apu->outPorts[0];
  if (!(cur > 0 && cur < 0xf0)) cur = g_last_p0;
  p->port_to_snes[0] = 0;
  p->input_ports[0] = (cur > 0 && cur < 0xf0) ? cur : 255;
  p->input_ports[1] = p->input_ports[2] = p->input_ports[3] = 0;

  g_wire_p = p;
  g_wire_variant = np->variant;
  g_ack[0] = g_ack[1] = g_ack[2] = 0;
  g_frac = 0;
  g_wire_on = 1;
  g_was_hle = 1;

  /* Bootstrap: Music_HandleCmdFromSnes is normally driven ONLY by
   * Spc_Loop_Part2's tempo-accumulator gate (`t = main_tempo_accum +
   * ticks*HIBYTE(tempo)`, dispatched when t wraps past 256). A freshly
   * zero-copy-adopted player has tempo=0 (CreateWithState memsets the whole
   * struct) -- with HIBYTE(tempo)==0 that gate can never wrap on its own, so
   * the input_ports[0] command set above would sit forever unconsumed and
   * the swapped-in player would go permanently silent (confirmed on real
   * Super Metroid: swap succeeds, but tempo/port_to_snes[0]/music_ptr_toplevel
   * all stay pinned at their zeroed reset values for the rest of the run).
   * Real hardware doesn't have this problem -- the SPC700's own driver-init
   * code runs unconditionally at boot and processes the first command outside
   * this steady-state per-tick gate, a bootstrap path this player (adopted
   * mid-stream, not booted) doesn't model. So pump the handler directly: one
   * call consumes input_ports[0] and starts "start_playing_sound" (sets
   * counter_sf0c=2); two more no-op calls (input_ports[0] is now the 255
   * sentinel) walk counter_sf0c down to 0, which is what actually enters the
   * song's phrase stream and processes its tempo-set command (spc_player.c's
   * `p->tempo = arg << 8`). After that the normal gate is self-sustaining. */
  if (cur > 0 && cur < 0xf0)
    for (int i = 0; i < 3; i++)
      Music_HandleCmdFromSnes(p);

  wire_mirror_ports(snes->apu, p);

  printf("[nspc] frame %d: swapped to native N-SPC (variant=%s song=%04x "
         "instr=%04x dir=%02x00 chOK=%d)\n",
         g_frame, np->variant, np->songList & 0xffff,
         g_nspc_cfg.instrTable & 0xffff, g_nspc_cfg.dirPage & 0xff, np->chOK);
}

/* ===================== TEMP diagnostic (test rig only, not called from the
 * product path — remove before shipping) ================================== */
/* Simulates "a savestate was taken while HLE was active" without needing a
 * full save/load file round-trip -- sets the one flag wire_restore_after_
 * load() gates on, so a test rig can call that function directly at a chosen
 * frame and observe whether it adopts a transient outPorts[0] safely. */
void wire_test_set_was_hle(int v) { g_was_hle = v; }

void wire_debug_dump(int frame) {
  if (!g_wire_on) { printf("[nspc-dbg] f=%d LLE g_last_p0=%02x\n", frame, g_last_p0); return; }
  SpcPlayer *p = g_wire_p;
  printf("[nspc-dbg] f=%d tempo=%04x main_tempo_accum=%02x port_to_snes0=%02x "
         "input0=%02x music_ptr=%04x counter_sf0c=%04x is_chan_on=%02x "
         "key_on=%02x key_off=%02x ffwd=%d ch0_ptr=%04x ch0_instr=%d ch0_vol=%d "
         "sfxTimerAccum=%d in1=%02x in2=%02x in3=%02x sfx1_cur=%d sfx1_pri=%d "
         "sfx2_cur=%d sfx2_pri=%d sfx3_cur=%d sfx3_pri=%d\n",
         frame, p->tempo, p->main_tempo_accum, p->port_to_snes[0],
         p->input_ports[0], p->music_ptr_toplevel, p->counter_sf0c, p->is_chan_on,
         p->key_ON, p->key_OFF, p->fast_forward,
         p->channel[0].pattern_order_ptr_for_chan, p->channel[0].instrument_id,
         p->channel[0].final_volume, p->sfx_timer_accum,
         p->input_ports[1], p->input_ports[2], p->input_ports[3],
         p->sfx1.cur_sound, p->sfx1.priority, p->sfx2.cur_sound, p->sfx2.priority,
         p->sfx3.cur_sound, p->sfx3.priority);
}

/* ===================== per-frame detection gate ========================== */
int wire_try_swap(Snes *snes, int frame) {
  g_frame = frame;
  if (g_wire_on || !g_wire_enable || !snes->apu) return 0;

  /* Track live outPorts[0] stability every frame -- see NSPC_SWAP_STABLE_
   * FRAMES comment above.  Runs unconditionally (not gated on frame % 60)
   * so the streak reflects real history by the time detection is ready. */
  {
    uint8_t p0 = snes->apu->outPorts[0];
    if (p0 == g_p0_last) {
      if (g_p0_stable < 0x7fffffff) g_p0_stable++;
    } else {
      g_p0_last = p0;
      g_p0_stable = 0;
    }
  }

  /* Fast re-resume after a savestate load (wire_restore_after_load() set
   * this instead of adopting outPorts[0] immediately -- see that function's
   * comment). The driver was already confirmed valid before the save, so
   * skip the slow two-checks-60-apart re-confirmation below; only the
   * stability gate (g_p0_stable, tracked above unconditionally every frame)
   * needs to clear before it's safe to adopt. */
  if (g_load_pending_resume) {
    if (g_p0_stable < NSPC_SWAP_STABLE_FRAMES) return 0;
    NspcParams np;
    if (!nspc_extract(snes->apu->ram, &np) || np.chOK < 6 || !strcmp(np.variant, "?") ||
        (strcmp(np.variant, "std") && strcmp(np.variant, "YI"))) {
      /* Driver signature gone or changed since the save (unexpected for a
       * same-game load) -- bail to the normal slow path rather than risk
       * adopting something unconfirmed. */
      g_load_pending_resume = 0;
      g_ok_streak = 0;
      return 0;
    }
    g_load_pending_resume = 0;
    wire_swap(snes, &np, snes->apu->ram);
    return 1;
  }

#ifndef NSPC_SWAP_MIN_FRAME
#define NSPC_SWAP_MIN_FRAME 120  /* test-only override, e.g. -DNSPC_SWAP_MIN_FRAME=300,
                                  * to wobble the detection window and confirm the
                                  * stability gate holds at any swap timing, not just
                                  * the one the ROM happens to produce by default. */
#endif
  if (frame < NSPC_SWAP_MIN_FRAME || (frame % 60) != 0) return 0;
  /* Driver must actually be running (upload done, PC out of IPL ROM). */
  if (snes->apu->spc->pc >= 0xffc0) { g_ok_streak = 0; return 0; }
  NspcParams np;
  if (!nspc_extract(snes->apu->ram, &np) || np.chOK < 6 ||
      !strcmp(np.variant, "?")) { g_ok_streak = 0; return 0; }
  /* Live PORT PROTOCOL is per-family.  Only the SM/ALttP-lineage std
   * protocol is implemented (instant-ack ports 1-3).  GD3/SMW/Tose/Intelli
   * have stateful driver responses that instant-acks cannot fake — they
   * stay LLE forever (fail-safe).  std and YI pass. */
  if (strcmp(np.variant, "std") && strcmp(np.variant, "YI")) {
    g_ok_streak = 0;
    return 0;
  }
  if (++g_ok_streak < 2) return 0;   /* stable across two checks 60 frames apart */
  if (g_p0_stable < NSPC_SWAP_STABLE_FRAMES) return 0;  /* handshake in flight; defer */
  wire_swap(snes, &np, snes->apu->ram);
  return 1;
}

/* ===================== ROM gate (no CRC — detection is ARAM-based) ======= */
bool wire_configure_rom(const uint8_t *rom, uint32_t len) {
  (void)rom; (void)len;
  /* Detection is ARAM-signature-based (wire_try_swap scans for the N-SPC
   * driver in live ARAM), NOT full-ROM CRC.  We just reset state and leave
   * detection active.  Non-N-SPC ROMs never trigger the swap. */
  g_wire_on = 0;
  g_wire_enable = 1;
  g_wire_p = NULL;
  g_wire_variant = "-";
  g_frac = 0;
  g_ok_streak = 0;
  g_was_hle = 0;
  g_last_p0 = 0;
  g_ack[0] = g_ack[1] = g_ack[2] = 0;
  g_p0_last = 0;
  g_p0_stable = 0;
  g_load_pending_resume = 0;
  return true;  /* detection armed; actual gate is in wire_try_swap */
}

/* ===================== savestate hooks =================================== */
/* prepare_save: NO-OP for generic N-SPC.  CopyVariablesToRam writes SM's
 * zero-page layout into foreign ARAM, which would corrupt a non-SM game's
 * data.  The native C struct is derived state — on load we rebuild it from
 * the (correctly serialized) ARAM.  The ARAM itself is serialized by the
 * core's snes_saveload (it dumps apu->ram). */
void wire_prepare_save(void) {
  /* intentionally empty — see comment above */
}

/* restore_after_load: do NOT adopt outPorts[0] immediately. The loaded value
 * may be a transient mid-handshake byte saved by chance -- the exact same
 * class of bug wire_swap()'s live-swap path had (see nspc-swap-transient-
 * fix-0720 memory: confirmed on Super Metroid, a snapshot-adopted transient
 * froze the CPU's poll for the next port-0 transition forever). Save/load is
 * MORE frequent than the once-per-boot live swap, so this path needs the
 * same defense, not a shortcut around it.
 * Instead: stay in LLE (the just-restored real SPC700 state resumes exactly
 * like any non-HLE game -- it doesn't need our help) and let wire_try_swap()
 * re-adopt once outPorts[0] has genuinely settled, via the fast g_load_
 * pending_resume path above (skips the slow driver re-detection, since it
 * was already confirmed before the save). */
void wire_restore_after_load(Snes *snes) {
  if (!g_was_hle || !g_wire_enable || !snes || !snes->apu)
    return;
  g_wire_on = 0;
  g_wire_p = NULL;
  g_p0_last = snes->apu->outPorts[0];
  g_p0_stable = 0;
  g_load_pending_resume = 1;
}
