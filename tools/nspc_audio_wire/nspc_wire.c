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
#include "src/snes/dsp_regs.h"
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

/* Upload mailbox state (mid-game bank reload) -- see handle_upload_write()
 * further below for the protocol this implements and why. */
enum { NSPC_UPLOAD_IDLE, NSPC_UPLOAD_READY, NSPC_UPLOAD_DATA };
static int g_upload_mode = NSPC_UPLOAD_IDLE;
static uint8_t g_upload_ports[4];
static uint16_t g_upload_addr;
static uint8_t g_upload_counter;
static bool g_upload_first_byte;

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

/* DIR-page-populated gate (EarthBound): the driver-signature detection above
 * only proves the N-SPC *code* is uploaded and running -- it says nothing
 * about whether the game's *sample data* has arrived. EarthBound's driver
 * passes chOK>=6 and settles by frame 180, but its DSP sample directory (the
 * DIR page nspc_extract() already recovers into np.dir) is confirmed
 * all-zero at that moment: a second, independent upload burst at frames
 * 343-345 is what actually registers samples. Swapping freezes the SPC700,
 * so that burst never runs and the game is silent forever after.
 * Fix: piggyback on the existing two-checks-60-frames-apart streak
 * (g_ok_streak) instead of adding a new cadence. A check only counts toward
 * the streak if the 256-byte DIR page is (a) not all zero and (b) identical
 * to the snapshot taken at the previous check -- i.e. stable, not mid-burst.
 * Any zero or changed page resets the streak, so the game re-arms and tries
 * again 60 frames later once the upload settles. */
static uint8_t g_dir_snapshot[256];
static int g_dir_snapshot_valid = 0;

static bool dir_page_all_zero(const uint8_t *ram, int dirAddr) {
  const uint8_t *page = &ram[dirAddr & 0xffff];
  for (int i = 0; i < 256; i++) if (page[i] != 0) return false;
  return true;
}

/* ===================== one 32 kHz sample step ============================ */
/* Exactly SpcPlayer_GenerateSamples' semantics: advance the 500 Hz N-SPC
 * driver tick (Spc_Loop_Part2 + Part1) when timer_cycles wraps, then one
 * dsp_cycle to produce one sample pair.
 * The sequencer tick is skipped while an upload mailbox transfer is in
 * flight (g_upload_mode != IDLE) -- matching smw_exact_wire.c's own
 * wire_frame_audio(), which calls bare dsp_cycle() during upload instead
 * of gen_samples()'s full tick. Without this, Music_HandleCmdFromSnes kept
 * running on its normal per-tick gate concurrently with the upload, and
 * could overwrite port_to_snes[0] with a stale pending command from BEFORE
 * the upload started (start_playing_sound fires whenever input_ports[0] is
 * a fresh, non-255, non-0xf1 value that differs from the current
 * port_to_snes[0]) -- clobbering the upload ack/counter echo the 65816 is
 * actively polling for. Confirmed empirically on Samurai Spirits: the
 * first two (rapid, back-to-back) upload cycles happened to not race this
 * way, but every game upload after that got the ack overwritten and hung
 * forever -- a real hardware SPC700 does not have this problem because it
 * physically jumps out of the music loop into a dedicated upload receiver
 * for the duration, so there is nothing left running to race with. */
static inline void wire_step_sample(SpcPlayer *p) {
  if (g_upload_mode == NSPC_UPLOAD_IDLE && p->timer_cycles >= 64) {
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
/* Forward declaration: defined further below (needs SpcPlayer/upload state
 * declared there), called from here. */
static bool handle_upload_write(Snes *snes, int port, uint8_t val);

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
  if (handle_upload_write(snes, port, val))
    return;
  if (port == 0) {
    /* ALttP-style mailboxes write 00 when idle; SM's engine reads a 0 that
     * differs from the current song as "stop".  Idle-zero is not a command
     * — drop it (games stop via 0xf0 pause / 0xf1 fade instead). */
    if (val != 0) {
      g_wire_p->input_ports[0] = val;
      /* FIRST SONG AFTER A SILENT SWAP.  wire_swap()'s bootstrap only pumps
       * Music_HandleCmdFromSnes when a song was ALREADY playing at swap time
       * (`cur > 0 && cur < 0xf0`). A game that is still silent then -- Zelda 3
       * swaps at frame 180 and does not command music until frame 355 -- never
       * gets that pump, and a zero-copy-adopted player has tempo == 0, so
       * Spc_Loop_Part2's gate (`ticks * HIBYTE(tempo)`) can never wrap and
       * nothing ever consumes this write. The command sits in input_ports[0]
       * for the rest of the run: permanent silence, on the device only.
       * The host harness does not show it because tools/nspc_audio_wire/wire.c
       * builds its player with SpcPlayer_Create + SpcPlayer_Initialize, which
       * leaves a non-zero tempo -- the two copies diverge exactly here.
       * Same three-call pump as the swap bootstrap, for the same reason. */
      if (g_wire_p->port_to_snes[0] == 0 && val < 0xf0)
        for (int i = 0; i < 3; i++)
          Music_HandleCmdFromSnes(g_wire_p);
    }
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
  /* No timed clear of the finish-counter echo here (earlier versions
   * cleared it back to 0 a fixed number of ticks after FINISH). Samurai
   * Spirits fires back-to-back upload cycles within a single real frame;
   * a timer sized for one cycle's pacing clears the echo before the CPU's
   * poll gets back around to reading it for the very next cycle (read
   * trace confirmed: the poll saw the PREVIOUS cycle's stale echo once,
   * then 0 forever -- the fresh echo was already timer-cleared). The next
   * real write (a new trigger's 0xaa, or the next finish's own counter)
   * overwrites this port naturally, so no schedule-based clear is needed. */
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

/* ===================== sequencer reset (shared) ============================ */
/* Clears native sequencer state (matching host wire.c post-Initialize).
 * Used both at initial adoption (wire_swap) and after a mid-game upload
 * completes (handle_upload_write) -- both cases restart playback from ARAM
 * that is now known-good, and both need the same clean slate. */
static void nspc_reset_sequencer_state(SpcPlayer *p) {
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
}

/* ===================== upload mailbox (mid-game bank reload) =============== */
/* N-SPC engines reload music/SFX banks mid-game by replacing the SPC-side
 * driver's data through a port mailbox handshake, not just once at boot --
 * confirmed empirically on Samurai Spirits via a sequential $2140-2143
 * trace (LLE reference): write 0xff on port 0 (trigger) -> engine echoes
 * 0xaa on port_to_snes[0] and 0xbb on port 1 (the CPU polls for port1==0xbb
 * specifically) -> CPU writes the upload address low/high on ports 2/3, a
 * transfer-mode byte on port 1, then 0xcc on port 0 to start the transfer
 * -> a counter (0,1,2,...) on port 0 paired with one data byte per counter
 * on port 1, ram[addr++] = data each time the counter matches expected+1.
 * Byte-for-byte identical to smw_exact_wire.c's handle_upload_write()
 * protocol, EXCEPT the trigger port: SMW's own driver uses port 1, this
 * dialect uses port 0 -- confirmed by direct trace comparison, not assumed.
 * The generic wire previously had no support for this at all: the trigger
 * byte (0xff) landed in input_ports[0] and got silently treated as a
 * "start playing song 0xff" request, which Music_HandleCmdFromSnes's own
 * sentinel check (a != 255) discards as a no-op (0xff == 255, colliding
 * with the "no command pending" marker) -- so nothing ever advanced the
 * SPC-side, and the CPU's poll for the ack spun forever. This was a
 * general protocol coverage gap in the generic wire, not the swap-timing
 * bug fixed in nspc-swap-transient-fix-0720 -- unrelated bug, found only
 * once sampling moved past the original three-game gate (see that memory
 * doc for the wider distinction).
 * State (g_upload_mode etc) is declared up in the state block above. */
static bool handle_upload_write(Snes *snes, int port, uint8_t val) {
  Apu *apu = snes->apu;
  if (g_upload_mode == NSPC_UPLOAD_IDLE) {
    if (port != 0 || val != 0xff)
      return false;
    g_upload_mode = NSPC_UPLOAD_READY;
    memset(g_upload_ports, 0, sizeof(g_upload_ports));
    g_wire_p->port_to_snes[0] = 0xaa;
    /* outPorts[1] mirrors g_ack[0] (see wire_mirror_ports), NOT
     * port_to_snes[1] -- ports 1-3 use the separate instant-ack array for
     * the normal SFX protocol. The upload ack needs to go through the same
     * path or the CPU's poll for port1==0xbb never resolves. */
    g_ack[0] = 0xbb;
    dsp_write(g_wire_p->dsp, FLG, 0x60);
    dsp_write(g_wire_p->dsp, KOF, 0xff);
    wire_mirror_ports(apu, g_wire_p);
    return true;
  }

  g_upload_ports[port] = val;
  if (g_upload_mode == NSPC_UPLOAD_READY) {
    if (port == 0 && val == 0xcc) {
      g_upload_addr = g_upload_ports[2] | (g_upload_ports[3] << 8);
      g_upload_counter = val;
      g_upload_first_byte = true;
      g_upload_mode = NSPC_UPLOAD_DATA;
      g_wire_p->port_to_snes[0] = val;
      wire_mirror_ports(apu, g_wire_p);
    }
    return true;
  }

  if (port != 0)
    return true;

  if (g_upload_first_byte) {
    g_upload_first_byte = false;
    g_upload_counter = val;
    g_wire_p->port_to_snes[0] = val;
    wire_mirror_ports(apu, g_wire_p);
    return true;
  }

  if (val == (uint8_t)(g_upload_counter + 1)) {
    g_wire_p->ram[g_upload_addr++] = g_upload_ports[1];
    g_upload_counter = val;
    g_wire_p->port_to_snes[0] = val;
    wire_mirror_ports(apu, g_wire_p);
    return true;
  }

  if (g_upload_ports[1] != 0) {
    /* Another block follows: this counter is the block handshake, the
     * first data byte arrives with the next counter write. */
    g_upload_addr = g_upload_ports[2] | (g_upload_ports[3] << 8);
    g_upload_counter = val;
    g_upload_first_byte = true;
    g_wire_p->port_to_snes[0] = val;
    wire_mirror_ports(apu, g_wire_p);
    return true;
  }

  /* port1=0 with a non-sequential counter means the transfer is done.
   * The uploaded ARAM region is now current -- reset the sequencer the
   * same way a fresh adoption does; the game's next port-0 song command
   * restarts playback from the (now correct) data.
   *
   * nspc_reset_sequencer_state() alone is NOT enough here: it only clears
   * per-channel/SFX runtime state, matching what wire_swap() needs. Finish
   * additionally has to match smw_exact_wire.c's SmwSpcPlayer_FinishRawUpload
   * (the proven-correct reference for this exact transition), which also
   * zeroes music_ptr_toplevel/input_ports/last_value_from_snes and clears
   * FLG to 0x20.
   *
   * ROOT CAUSE of the Samurai Spirits stall-after-5-uploads, confirmed by
   * reading the generated driver (spc_player.c's Music_HandleCmdFromSnes):
   * two tries below this comment (input_ports[0]=255 instead of 0, and
   * dropping the finish-echo's timed auto-clear) both measured ZERO effect
   * on STATEHASH/AUDIOHASH -- the real mechanism is different. Setting
   * port_to_snes[0]=val (the raw upload-protocol counter byte, e.g. 0x09)
   * doubles as the driver's OWN "a song is active" flag: the very next
   * Music_HandleCmdFromSnes tick skips its early "port_to_snes[0]==0 ->
   * return" gate (since val != 0), falls into the "process next phrase"
   * path once counter_sf0c decrements to 0, reads music_ptr_toplevel as a
   * real sequence pointer, hits t==0 on the garbage there, and takes that
   * engine's own "a=0 -> start_playing_sound -> port_to_snes[0]=0" branch
   * -- clobbering the echo via the DRIVER's own logic, not any race in
   * this file. counter_sf0c is what gates reaching that path; holding it
   * away from 0 keeps the driver idle until a real start_playing_sound
   * (which sets counter_sf0c=2 itself) supersedes it. */
  nspc_reset_sequencer_state(g_wire_p);
  g_wire_p->music_ptr_toplevel = 0;
  g_wire_p->counter_sf0c = 0xffff;
  memset(g_wire_p->input_ports, 0, sizeof(g_wire_p->input_ports));
  g_wire_p->input_ports[0] = 255;
  memset(g_wire_p->last_value_from_snes, 0, sizeof(g_wire_p->last_value_from_snes));
  g_wire_p->port_to_snes[1] = g_wire_p->port_to_snes[2] = g_wire_p->port_to_snes[3] = 0;
  g_ack[0] = g_ack[1] = g_ack[2] = 0;
  dsp_write(g_wire_p->dsp, FLG, 0x20);
  g_upload_mode = NSPC_UPLOAD_IDLE;
  g_wire_p->port_to_snes[0] = val;
  wire_mirror_ports(apu, g_wire_p);
  return true;
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
  /* +2, because detection and the player index the song table from different
   * origins. The driver's own instruction is `MOV A,!base+X` with X = song*2,
   * so `base` is the entry for song 0 and that absolute address is what
   * nspc_extract() correctly recovers. Every decompiled N-SPC player instead
   * writes `table + (song-1)*2`, i.e. the entry for song 1 -- one entry, two
   * bytes, higher. Two independent decompilations agree:
   *   Zelda 3        extract 0xcffe   external/zelda3/spc_player.c:737  0xD000
   *   Super Metroid  extract 0x581e   external/sm/src/spc_player.c:865  0x5820
   * and nspc_config.h's own SM defaults say the same (songList 0x5820,
   * songCur 0x581e -- detection was handing over songCur as songList).
   * Measured on the host wire harness, 800/1800 frames vs LLE:
   *   Zelda 3       rms 152 -> 1608 (LLE 1586) and the framebuffer hash
   *                 becomes identical to LLE for the whole run
   *   Super Metroid rms   0 ->  191 (LLE 1692) -- audible, not yet right
   *   EarthBound / Mega Man X: silent before and after; NOT fixed by this.
   * So this is a real defect in the common path, not a general std cure --
   * SNES_NSPC_HLE stays default-off. */
  g_nspc_cfg.songList   = np->songList + 2;
  g_nspc_cfg.songCur    = np->songList;
  g_nspc_cfg.dirPage    = ((np->dir >= 0 ? np->dir : 0x6d00) >> 8) & 0xff;

  /* Zero-copy adoption: reuse the live APU ARAM and DSP.  NO memcpy, NO
   * SpcPlayer_Initialize — Vector_Reset_Spc memsets ARAM regions that
   * belong to the foreign game's data, and dsp_reset would wipe the live
   * DSP.  The native C struct starts zeroed (CreateWithState memsets it),
   * then we set the fields the song-start needs. */
  SpcPlayer *p = SpcPlayer_CreateWithState((uint8 *)aram, snes->apu->dsp);
  nspc_reset_sequencer_state(p);

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

/* ===================== commanded-song table-entry sanity gate ============
 * chOK (nspc_score_songlist) only probes 4 candidate SECTION addresses near
 * the detected songList base -- it never checks the slot the CURRENTLY
 * COMMANDED song actually indexes. On at least one ROM (Mega Man X, real
 * song id 49) the detected base scores chOK>=6 from a handful of plausible-
 * looking entries near its start, but the table is far shorter than the
 * real one: everything past roughly the first 8-9 slots reads literal
 * 0x0000. Song 49's slot lands in that dead zone. wire_swap()'s bootstrap
 * then does exactly what real N-SPC firmware does with a literal top-level
 * word of 0 -- treats it as the documented "a=0" restart -- which sets
 * port_to_snes[0]=0 and returns; nothing ever un-latches it because no
 * channel is left active to trigger another restart, so the swap goes
 * permanently and silently stuck (confirmed on the device rig: STATEHASH/
 * AUDIOHASH freeze within ~2 detection cycles of the swap). That is not a
 * pacing bug and not a missing a==0 special case -- the a==0 handling is
 * exactly what the real driver does too -- it is the songList base itself
 * being unreliable for this ROM. Rather than guess a better base (needs
 * real per-ROM disassembly, out of scope here), refuse to commit to a swap
 * whose own about-to-be-played song already reads as empty. This has to be
 * stricter than chOK's own "0 or in-range" test: a run of literal zeros --
 * exactly the dead-zone content that causes the freeze -- passes that test
 * too (0 counts as an "ok" channel-off pointer), so it cannot tell a real
 * silent section from a wrong table base. Require actual content instead:
 * at least half of the 8 channel words must be real nonzero in-range
 * pointers, not merely "not garbage". */
static bool wire_song_table_entry_sane(const uint8_t *ram, int songListBase, uint8_t cur) {
  if (!(cur > 0 && cur < 0xf0)) return true;  /* nothing concrete to validate yet */
  int sec = rd16(ram, songListBase + cur * 2);
  if (sec < 0x100 || sec >= 0xfff0) return false;
  int t = sec, nonzero_ok = 0;
  for (int ch = 0; ch < 8; ch++) {
    int cp = rd16(ram, t); t += 2;
    if (cp >= 0x100 && cp < 0xffff) nonzero_ok++;
  }
  /* "0 or in-range" (chOK's own test) passes on a run of literal zeros too --
   * exactly the all-channels-off dead zone that produces this bug's freeze
   * in the first place (confirmed on the device rig: an all-zero 8-word
   * read scores perfectly under that test). Require real content: at least
   * half the channels must be actual nonzero in-range pointers, not just
   * "not garbage". Keep in step with wire.c's copy. */
  return nonzero_ok >= 4;
}

/* ===================== per-frame detection gate ========================== */
int g_real_frame = 0;  /* DEBUG ONLY: unlike g_frame (a write-event counter
                         * despite its name), this is the actual video-frame
                         * number, for debug-print correlation. */
int wire_try_swap(Snes *snes, int frame) {
  g_frame = frame;
  g_real_frame = frame;
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
    {
      uint8_t cur = snes->apu->outPorts[0];
      if (!(cur > 0 && cur < 0xf0)) cur = g_last_p0;
      if (!wire_song_table_entry_sane(snes->apu->ram, np.songList, cur)) {
        g_load_pending_resume = 0;
        g_ok_streak = 0;
        return 0;
      }
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
  /* Assets-uploaded gate (see g_dir_snapshot comment above): the driver
   * being detected is not the same thing as its sample directory being
   * populated. Require np.dir to be resolved, non-empty, and byte-identical
   * to the snapshot taken at the PREVIOUS 60-frame check -- reusing the same
   * streak this loop already maintains, so a stable DIR page still swaps at
   * the normal two-check cadence and a mid-upload or not-yet-uploaded DIR
   * page resets the streak instead of swapping into silence. */
  if (np.dir < 0 || dir_page_all_zero(snes->apu->ram, np.dir)) {
    g_ok_streak = 0;
    g_dir_snapshot_valid = 0;
    return 0;
  }
  if (g_dir_snapshot_valid && memcmp(g_dir_snapshot, &snes->apu->ram[np.dir & 0xffff], 256) != 0)
    g_ok_streak = 0;   /* DIR changed since last check -- upload still in flight */
  memcpy(g_dir_snapshot, &snes->apu->ram[np.dir & 0xffff], 256);
  g_dir_snapshot_valid = 1;
  if (++g_ok_streak < 2) return 0;   /* stable across two checks 60 frames apart */
  if (g_p0_stable < NSPC_SWAP_STABLE_FRAMES) return 0;  /* handshake in flight; defer */
  {
    uint8_t cur = snes->apu->outPorts[0];
    if (!(cur > 0 && cur < 0xf0)) cur = g_last_p0;
    if (!wire_song_table_entry_sane(snes->apu->ram, np.songList, cur)) {
      /* The detected songList base does not survive the one check that
       * matters -- the actual commanded song's own slot. Treat exactly like
       * a chOK failure: reset the streak and keep re-trying LLE-only: if the
       * ROM's real table is elsewhere this stays permanently unswapped
       * (fail-safe, same outcome as detection never succeeding at all). */
      g_ok_streak = 0;
      return 0;
    }
  }
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
  g_upload_mode = NSPC_UPLOAD_IDLE;
  g_upload_first_byte = false;
  g_dir_snapshot_valid = 0;
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
