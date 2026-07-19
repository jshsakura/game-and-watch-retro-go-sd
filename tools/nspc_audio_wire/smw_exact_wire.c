/* Live SMW audio HLE using the project's exact SMW SPC reimplementation.
 *
 * The generic N-SPC wire adapts Super Metroid's player to several sequence
 * dialects.  That is sufficient for offline playback, but not for SMW's live
 * four-port protocol.  SMW's own decomp already has the exact music, SFX and
 * port implementation, so adopt the running LLE state into that player and
 * stop executing the SPC700 only after the upload has completed.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "src/snes/snes.h"
#include "src/snes/apu.h"
#include "src/snes/spc.h"
#include "src/snes/dsp.h"
#include "src/snes/dsp_regs.h"
#include "../../external/smw/src/smw_spc_player.h"
#include "snes_driver_sigs.h"
#include "wire.h"

/* Exported by the generated, zero-copy SMW player (build_smw_exact.sh). */
SpcPlayer *SmwSpcPlayer_CreateWithState(uint8 *ram, Dsp *dsp);
void SmwSpcPlayer_FinishRawUpload(SpcPlayer *p);

/* Original apu_run, renamed in a generated copy of apu.c. */
void apu_run_lle(Apu *apu, int cyclesToRun);

int g_wire_on;
int g_wire_enable = 1;          /* master switch; detection stays ARAM-based */
const char *g_wire_variant = "-";

static SpcPlayer *g_player;
static Apu *g_apu;
static int g_detect_streak;
static int g_frame;
static int g_upload_request_frame = -1;

enum { UPLOAD_IDLE, UPLOAD_READY, UPLOAD_DATA };
static int g_upload_mode;
static uint8_t g_upload_ports[4];
static uint16_t g_upload_addr;
static uint8_t g_upload_counter;
static bool g_upload_first_byte;
static bool g_upload_finish_ack;
static void adopt_lle_state(Snes *snes);

bool wire_configure_rom(const uint8_t *rom, uint32_t len) {
  /* Identity is established by the ARAM driver signature (ptnJumpToVcmdSMW),
   * not by a full-ROM hash.  The SMW sound engine is byte-identical across
   * the US vanilla image, Korean translations, bug-fix hacks and most
   * level packs — they all upload the same N-SPC driver blob to ARAM, where
   * wire_try_swap() detects it via SNES_DRIVER_SIGS.  A full-ROM CRC (or an
   * exact-title compare) would deny HLE to every translated or hacked SMW,
   * and to boot would be plain wrong: the real internal title has no space
   * between MARIO and WORLD ("SUPER MARIOWORLD", external/sm's
   * spin_skip.c:156), not "SUPER MARIO WORLD" — a 21-byte compare against
   * that never matched vanilla SMW at all (g_wire_enable stuck at 0, pure
   * SPC700 LLE, 55.4fps -> 46fps).  The LoROM internal-title field (0x7FC0)
   * is now only a cheap boot-log hint; detection itself is unconditionally
   * armed and stays ARAM-based so a title-hacked ROM is still covered by
   * has_smw_driver() in wire_try_swap(). */
  static const char smw_title_prefix[16] = "SUPER MARIOWORLD";
  const uint8_t *title = (len >= 0x7FD0u) ? rom + 0x7FC0u : NULL;
  bool title_hint = title && memcmp(title, smw_title_prefix, sizeof(smw_title_prefix)) == 0;
  g_wire_enable = 1;
  g_wire_on = 0;
  g_player = NULL;
  g_apu = NULL;
  g_detect_streak = 0;
  g_upload_request_frame = -1;
  g_upload_mode = UPLOAD_IDLE;
  g_upload_finish_ack = false;
  g_frame = 0;
  return title_hint;
}

void wire_prepare_save(void) {
  if (g_wire_on && g_player)
    g_player->copy_vars(g_player, true);
}

void wire_restore_after_load(Snes *snes) {
  if (!g_wire_enable || !snes || !snes->apu)
    return;
  /* prepare_save serialized native variables into the shared ARAM; rebuild
   * the private ~600-byte C state and continue without running stale SPC CPU. */
  g_upload_mode = UPLOAD_IDLE;
  g_upload_finish_ack = false;
  g_upload_request_frame = -1;
  g_detect_streak = 0;
  adopt_lle_state(snes);
}

static void mirror_ports(Apu *apu) {
  memcpy(apu->outPorts, g_player->port_to_snes, 4);
}

/* Emulate SMW's standard APU block-upload mailbox after the SPC700 has been
 * replaced.  The game can reload music banks at runtime; merely freezing the
 * SPC makes its 65816 upload loop wait forever.  We write the exact byte stream
 * directly into the shared ARAM and reproduce the AA/BB + counter echoes. */
static bool handle_upload_write(int port, uint8_t val) {
  if (g_upload_mode == UPLOAD_IDLE) {
    if (port != 1 || val != 0xff)
      return false;
    g_upload_mode = UPLOAD_READY;
    memset(g_upload_ports, 0, sizeof(g_upload_ports));
    g_player->port_to_snes[0] = 0xaa;
    g_player->port_to_snes[1] = 0xbb;
    g_player->port_to_snes[2] = g_player->port_to_snes[3] = 0;
    dsp_write(g_player->dsp, FLG, 0x60);
    dsp_write(g_player->dsp, KOF, 0xff);
    mirror_ports(g_apu);
    return true;
  }

  g_upload_ports[port] = val;
  if (g_upload_mode == UPLOAD_READY) {
    if (port == 0 && val == 0xcc) {
      g_upload_addr = g_upload_ports[2] | (g_upload_ports[3] << 8);
      g_upload_counter = val;
      g_upload_first_byte = true;
      g_upload_mode = UPLOAD_DATA;
      g_player->port_to_snes[0] = val;
      mirror_ports(g_apu);
    }
    return true;
  }

  if (port != 0)
    return true;

  if (g_upload_first_byte) {
    /* First counter after CC is the block handshake (normally zero); port1
     * still contains the transfer-mode byte, not payload. */
    g_upload_first_byte = false;
    g_upload_counter = val;
    g_player->port_to_snes[0] = val;
    mirror_ports(g_apu);
    return true;
  }

  if (val == (uint8_t)(g_upload_counter + 1)) {
    g_player->ram[g_upload_addr++] = g_upload_ports[1];
    g_upload_counter = val;
    g_player->port_to_snes[0] = val;
    mirror_ports(g_apu);
    return true;
  }

  if (g_upload_ports[1] != 0) {
    /* Another block follows.  This counter is the block handshake; the first
     * data byte arrives with the next counter write. */
    g_upload_addr = g_upload_ports[2] | (g_upload_ports[3] << 8);
    g_upload_counter = val;
    g_upload_first_byte = true;
    g_player->port_to_snes[0] = val;
    mirror_ports(g_apu);
    return true;
  }

  /* port1=0 with a non-sequential counter means execute at port2:3.  Native
   * code does not execute the uploaded SPC binary; reset its exact sequencer
   * state the same way SmwSpcPlayer_Upload does after copying a block. */
  SmwSpcPlayer_FinishRawUpload(g_player);
  g_upload_mode = UPLOAD_IDLE;
  /* The IPL receiver acknowledges the final non-sequential counter before
   * jumping to the uploaded entry point.  Keep it visible for the 65816's
   * immediate polling loop; clear it on the next audio tick like driver init. */
  g_player->port_to_snes[0] = val;
  g_upload_finish_ack = true;
  mirror_ports(g_apu);
#ifndef SNES_SMW_HLE_PRODUCT
  if (getenv("WIRE_TRACE"))
    fprintf(stderr, "[smw-upload] f=%d complete entry=%02x%02x\n",
            g_frame, g_upload_ports[3], g_upload_ports[2]);
#endif
  return true;
}

void apu_run(Apu *apu, int cyclesToRun) {
  if (!g_wire_on) {
    apu_run_lle(apu, cyclesToRun);
    return;
  }
  /* The exact player advances when the frontend requests the frame's audio,
   * matching external/smw's production integration.  Native ticks and upload
   * writes already publish ports, so the very frequent CPU catch-up calls have
   * no work left; snes_catchupApu still consumes their fractional cycle debt. */
  (void)apu;
  (void)cyclesToRun;
}

void wire_apu_write(Snes *snes, uint32_t adr, uint8_t val) {
  snes_catchupApu(snes);
  int port = adr & 3;
  if (!g_wire_on) {
#ifndef SNES_SMW_HLE_PRODUCT
    if (getenv("WIRE_TRACE") && val)
      fprintf(stderr, "[lle-port] f=%d p%d<=%02x out=%02x,%02x,%02x,%02x pc=%04x\n",
              g_frame, port, val,
              snes->apu->outPorts[0], snes->apu->outPorts[1],
              snes->apu->outPorts[2], snes->apu->outPorts[3],
              snes->apu->spc->pc);
#endif
    snes->apu->inPorts[port] = val;
    /* Vanilla SMW replaces its boot/title APU image just before gameplay.
     * $ff on port 1 enters the driver's upload receiver.  Swapping before this
     * event strands the 65816 in its byte-transfer handshake; wait until the
     * real SPC700 has completed it, then adopt the final gameplay image. */
    if (port == 1 && val == 0xff)
      g_upload_request_frame = g_frame;
    return;
  }
#ifndef SNES_SMW_HLE_PRODUCT
  if (getenv("WIRE_TRACE") && val)
    fprintf(stderr, "[smw-port] f=%d p%d<=%02x out=%02x,%02x,%02x,%02x pc=%04x\n",
            g_frame, port, val,
            g_player->port_to_snes[0], g_player->port_to_snes[1],
            g_player->port_to_snes[2], g_player->port_to_snes[3],
            snes->apu->spc->pc);
#endif
  if (handle_upload_write(port, val))
    return;
  /* Do not instant-ack.  SMW detects edges and maintains independent music
   * and SFX state on all four ports. */
  g_player->input_ports[port] = val;
}

void wire_frame_audio(int16_t *buf, int n) {
  if (g_upload_mode == UPLOAD_IDLE) {
    if (g_upload_finish_ack) {
      g_player->port_to_snes[0] = 0;
      g_player->port_to_snes[1] = 0;
      g_upload_finish_ack = false;
    }
    g_player->gen_samples(g_player);
  } else {
    while (g_player->dsp->sampleOffset < 534)
      dsp_cycle(g_player->dsp);
  }
  dsp_getSamples(g_player->dsp, buf, n, 1);
  /* The native loop consumes commands and updates acknowledgements while
   * producing samples, so publish the resulting state after the tick. */
  /* g_player shares the live APU DSP and ARAM; only ports need mirroring. */
  mirror_ports(g_apu);
}

static bool sig_matches_at(const uint8_t *ram, int pos, const DriverSig *s) {
  for (int i = 0; i < s->len; i++)
    if (s->mask[i] == 'x' && ram[pos + i] != s->bytes[i])
      return false;
  return true;
}

static bool has_smw_driver(const uint8_t *ram) {
  for (int i = 0; i < SNES_DRIVER_SIG_COUNT; i++) {
    const DriverSig *s = &SNES_DRIVER_SIGS[i];
    if (strcmp(s->name, "ptnJumpToVcmdSMW"))
      continue;
    for (int pos = 0, last = 0x10000 - s->len; pos <= last; pos++)
      if (sig_matches_at(ram, pos, s))
        return true;
  }
  return false;
}

static void adopt_lle_state(Snes *snes) {
  Apu *apu = snes->apu;

  /* Zero-copy is essential on the device: ARAM is already 64 KiB in AHB.
   * The generated player stores a pointer instead of a second inline ARAM and
   * reuses the live DSP object.  copy_vars(false) reconstructs every native
   * sequencer/SFX field from the exact SMW zero-page layout. */
  g_player = SmwSpcPlayer_CreateWithState(apu->ram, apu->dsp);
  g_apu = apu;
  memcpy(g_player->input_ports, apu->inPorts, 4);
  g_player->copy_vars(g_player, false);
  g_wire_variant = "SMW-exact";
  g_wire_on = 1;
  mirror_ports(apu);
}

int wire_try_swap(Snes *snes, int frame) {
  g_frame = frame;
  if (g_wire_on || !g_wire_enable || !snes->apu)
    return 0;
#ifdef SNES_SMW_HLE_PRODUCT
  const int min_frame = 120;
#else
  const char *sf = getenv("WIRE_SWAP_FRAME");
  int min_frame = sf ? atoi(sf) : 120;
#endif
  if (frame < min_frame || frame % 60)
    return 0;
#ifdef SNES_SMW_HLE_PRODUCT
  if (g_upload_request_frame < 0 || frame - g_upload_request_frame < 60)
#else
  if (!getenv("WIRE_ALLOW_EARLY") &&
      (g_upload_request_frame < 0 || frame - g_upload_request_frame < 60))
#endif
    return 0;
  if (snes->apu->spc->pc >= 0xffc0 || !has_smw_driver(snes->apu->ram)) {
    g_detect_streak = 0;
    return 0;
  }
  if (++g_detect_streak < 2)
    return 0;

  adopt_lle_state(snes);
  fprintf(stderr, "[wire] frame %d: adopted exact SMW SPC state (zero-copy ARAM/DSP)\n", frame);
  return 1;
}
