/* Coexistence dispatcher for SNES audio HLE.
 *
 * SMW's dedicated exact wire (smw_exact_wire.c) and the generic N-SPC wire
 * (nspc_wire.c) each define apu_run, wire_apu_write, wire_try_swap,
 * wire_frame_audio, wire_configure_rom, wire_restore_after_load,
 * wire_prepare_save and the g_wire_on/g_wire_enable/g_wire_variant globals.
 * Same names in both, and only one implementation may occupy those symbols
 * in the final link. This file is compiled ONLY when both SNES_SMW_HLE=1
 * and SNES_NSPC_HLE=1 (Makefile.common's SNES_HLE_COEXIST); the Makefile
 * compiles the other two with those names renamed to an smwx prefix and an
 * nspc prefix respectively, via objcopy redefine-syms passes (smwx_redefines,
 * nspc_wire_coexist_redefines) applied before the normal snes_redefines
 * pass -- see Makefile.common. Neither of their own source files needed a
 * single line changed for this. This file alone owns the plain names
 * main_snes.c calls.
 *
 * Engine fingerprint, never full-ROM hash: try the SMW-exact backend first
 * (its own upload-completion plus ptnJumpToVcmdSMW ARAM signature gate,
 * unchanged), and only if that backend never engages does the generic
 * std-or-YI detector get a turn. This is not a race: SMW own generic-dialect
 * variant tag, SMW, is already excluded from the generic wire's std-or-YI
 * gate in nspc_wire.c wire_try_swap, and the exact wire has_smw_driver
 * signature never matches a non-SMW ROM, so at most one backend own
 * detector can ever match a given ROM. Exactly one backend ever becomes
 * active per run; if neither engine is recognized, both backends already
 * fall back to LLE forever on their own -- this file adds no new
 * unsupported-protocol case, it only decides which backend own fallback
 * applies.
 */
#include <stdint.h>
#include <stdbool.h>
#include "src/snes/snes.h"
#include "src/snes/apu.h"

/* -- SMW-exact backend (renamed via smwx_redefines) -- */
void smwx_apu_run(Apu *apu, int cyclesToRun);
void smwx_wire_apu_write(Snes *snes, uint32_t adr, uint8_t val);
int  smwx_wire_try_swap(Snes *snes, int frame);
void smwx_wire_frame_audio(int16_t *buf, int n);
bool smwx_wire_configure_rom(const uint8_t *rom, uint32_t len);
void smwx_wire_restore_after_load(Snes *snes);
void smwx_wire_prepare_save(void);
extern int smwx_g_wire_on;
extern const char *smwx_g_wire_variant;

/* -- generic N-SPC backend (renamed via nspc_wire_coexist_redefines) -- */
void nspc_apu_run(Apu *apu, int cyclesToRun);
void nspc_wire_apu_write(Snes *snes, uint32_t adr, uint8_t val);
int  nspc_wire_try_swap(Snes *snes, int frame);
void nspc_wire_frame_audio(int16_t *buf, int n);
bool nspc_wire_configure_rom(const uint8_t *rom, uint32_t len);
void nspc_wire_restore_after_load(Snes *snes);
void nspc_wire_prepare_save(void);
extern int nspc_g_wire_on;
extern const char *nspc_g_wire_variant;

/* -- shared LLE interpreter (one generated apu_wire.c copy, reused by both
 * backends' own apu_run() when THEY are the active/checked one; see below
 * for why the dispatcher must call this directly rather than delegate to
 * either backend's apu_run() while neither is locked in yet). -- */
extern void apu_run_lle(Apu *apu, int cyclesToRun);

typedef enum { BACKEND_NONE, BACKEND_SMW, BACKEND_NSPC } wire_backend_t;
static wire_backend_t g_active = BACKEND_NONE;

/* Plain names main_snes.c actually calls/reads. */
int         g_wire_on = 0;
int         g_wire_enable = 1;
const char *g_wire_variant = "-";

bool wire_configure_rom(const uint8_t *rom, uint32_t len) {
  g_active = BACKEND_NONE;
  g_wire_on = 0;
  g_wire_variant = "-";
  /* Both backends stay armed; wire_try_swap() below decides which (if
   * either) engages. Only SMW's return value carries real information (a
   * title-hint used purely for boot-log diagnostics) -- the generic wire's
   * wire_configure_rom() always returns true (detection armed, not a
   * match), so it isn't a meaningful "hint" the way SMW's is. */
  bool smw_title_hint = smwx_wire_configure_rom(rom, len);
  nspc_wire_configure_rom(rom, len);
  g_wire_enable = 1;
  return smw_title_hint;
}

void wire_prepare_save(void) {
  if (g_active == BACKEND_SMW) smwx_wire_prepare_save();
  else if (g_active == BACKEND_NSPC) nspc_wire_prepare_save();
}

void wire_restore_after_load(Snes *snes) {
  /* Forward to whichever backend is (or was, before this load) active.
   * If neither had engaged yet, both backends' own restore_after_load()
   * are no-ops when their own "was HLE active at save time" flag is unset
   * -- matching the pre-coexistence behavior exactly. */
  if (g_active == BACKEND_SMW) {
    smwx_wire_restore_after_load(snes);
    g_wire_on = smwx_g_wire_on;
    g_wire_variant = smwx_g_wire_variant;
  } else if (g_active == BACKEND_NSPC) {
    nspc_wire_restore_after_load(snes);
    g_wire_on = nspc_g_wire_on;
    g_wire_variant = nspc_g_wire_variant;
  }
}

int wire_try_swap(Snes *snes, int frame) {
  if (g_active == BACKEND_SMW) {
    int r = smwx_wire_try_swap(snes, frame);
    g_wire_on = smwx_g_wire_on;
    g_wire_variant = smwx_g_wire_variant;
    return r;
  }
  if (g_active == BACKEND_NSPC) {
    int r = nspc_wire_try_swap(snes, frame);
    g_wire_on = nspc_g_wire_on;
    g_wire_variant = nspc_g_wire_variant;
    return r;
  }
  /* Neither locked yet: poll SMW first (preferred when it applies), then
   * generic. Both backends' own per-frame stability/detection bookkeeping
   * (g_p0_stable tracking, g_detect_streak, upload-event sniffing, ...)
   * needs to run every frame regardless of which one eventually engages,
   * so both get called every frame until one locks in. */
  int r = smwx_wire_try_swap(snes, frame);
  if (smwx_g_wire_on) {
    g_active = BACKEND_SMW;
    g_wire_on = 1;
    g_wire_variant = smwx_g_wire_variant;
    return r;
  }
  r = nspc_wire_try_swap(snes, frame);
  if (nspc_g_wire_on) {
    g_active = BACKEND_NSPC;
    g_wire_on = 1;
    g_wire_variant = nspc_g_wire_variant;
    return r;
  }
  return 0;
}

void wire_apu_write(Snes *snes, uint32_t adr, uint8_t val) {
  if (g_active == BACKEND_SMW) { smwx_wire_apu_write(snes, adr, val); return; }
  if (g_active == BACKEND_NSPC) { nspc_wire_apu_write(snes, adr, val); return; }
  /* Neither locked yet: both backends need to see every port write during
   * LLE (SMW sniffs the upload-replace event, generic sniffs the last real
   * song command and outPorts[0] stability) -- forward to both. Each one's
   * own wire_apu_write(), when its own g_wire_on is 0, only does private
   * bookkeeping plus writing the SAME value into snes->apu->inPorts[port]
   * (idempotent), so calling both is side-effect-free beyond that
   * bookkeeping. */
  smwx_wire_apu_write(snes, adr, val);
  nspc_wire_apu_write(snes, adr, val);
}

void wire_frame_audio(int16_t *buf, int n) {
  if (g_active == BACKEND_SMW) { smwx_wire_frame_audio(buf, n); return; }
  if (g_active == BACKEND_NSPC) { nspc_wire_frame_audio(buf, n); return; }
  /* Unreachable in practice: main_snes.c only calls this when g_wire_on. */
}

void apu_run(Apu *apu, int cyclesToRun) {
  if (g_active == BACKEND_SMW) { smwx_apu_run(apu, cyclesToRun); return; }
  if (g_active == BACKEND_NSPC) { nspc_apu_run(apu, cyclesToRun); return; }
  /* Neither locked yet: do NOT call both backends' apu_run() here like
   * wire_apu_write() above -- unlike that bookkeeping-only call, each
   * backend's apu_run(), when its own g_wire_on is 0, advances the shared
   * LLE interpreter by cyclesToRun via apu_run_lle(). Calling it through
   * both backends would double-advance emulated time. Call the shared
   * interpreter directly instead. */
  apu_run_lle(apu, cyclesToRun);
}
