#!/usr/bin/env python3
"""Generate a Ledger-B-instrumented copy of external/sm's snes.c.

WHY A GENERATED COPY, NOT AN EDIT
---------------------------------
external/sm is a submodule. CLAUDE.md: do not edit files under external/ --
the build's dirty-submodule check rejects it, and a probe is exactly the kind
of change that must not leak into the core's history. The same trick already
exists for the APU cost split (tools/snes_diag/instrument.py), so this follows
it: copy the file into $(BUILD_DIR)/snes_prof/ with the probes injected and
compile that instead. SNES_DEVICE_PROFILE=0 never runs this script and the
release build is byte-identical to a tree without it.

WHAT IT INJECTS, AND WHY ONLY THIS MUCH
---------------------------------------
Two COARSE scopes, both children of run_frame_events():

  snes_catchupApu()  -> APU LLE exclusive cycles
  ppu_runLine(...)   -> PPU inclusive cycles

They must be the two that matter and no more. An adversarial review
(tools/snes_survey/snes_device_dwt_design_adversarial_review.md) measured
earlier fine-grained APU probing at 0.38-8.37% of baseline -- the same size as
the effect being hunted -- so there is deliberately no per-opcode and no
per-DSP-tick bracket here. Call counts are recorded so a frequency surprise is
visible in the dump instead of silently inflating the numbers.

snes_catchupApu is wrapped rather than opened at the top of its body because it
has an early return; a wrapper cannot leak a scope on a path a body edit would
miss. The wrapper is what gets the probe, so EVERY caller is covered -- and
that is the whole point: the end-of-frame catch-up in main_snes.c is the
visible one, but most of the APU's work is dragged in from inside
cpu_runOpcode() via snes_readBBus()/RtlApuWrite() on $2140-$2143. Bracketing
only the visible call would undercount the recoverable cost by most of it.

The two scopes are SIBLINGS, never nested (ppu_runLine does not reach the APU
and snes_catchupApu does not reach the PPU), which is what makes them safe to
subtract without a stack-based exclusive profiler. That claim is not assumed:
snes_prof_scope_enter/exit maintain a depth counter and the dump FAILS the run
if depth ever exceeds 1.

Usage: instrument.py <path/to/snes.c> <outdir>
"""
import os
import sys

APU_ANCHOR = "void snes_catchupApu(Snes* snes) {"
APU_REPLACEMENT = """/* --- SNES_DEVICE_PROFILE: Ledger B APU scope (generated, see
 * tools/snes_prof/instrument.py). The real body is renamed and a probed
 * wrapper keeps the original external name, so every caller -- including the
 * ones inside cpu_runOpcode()'s B-bus path -- is measured. */
static void snes_prof_catchupApu_real(Snes* snes);

void snes_catchupApu(Snes* snes) {
  uint32_t snes_prof_apu_t0__ = SNES_PROF_APU_SCOPE_ENTER();
  snes_prof_catchupApu_real(snes);
  SNES_PROF_APU_SCOPE_EXIT(snes_prof_apu_t0__);
}

static void snes_prof_catchupApu_real(Snes* snes) {"""

PPU_ANCHOR = "ppu_runLine(snes->ppu, snes->vPos);"
PPU_REPLACEMENT = "SNES_PROF_PPU_CALL(ppu_runLine(snes->ppu, snes->vPos));"

# $420B's synchronous general-DMA drain -- the only place general DMA runs, and
# it runs inside cpu_runOpcode(). Bracket it so its cycles come back out of
# cpu_only (which otherwise counts DMA as "interpreter"). The whole while loop is
# the macro argument; SNES_PROF_DMA_CALL takes back the APU work a DMA to $2140-3
# triggers, so the bucket is DMA exclusive of APU.
DMA_ANCHOR = "while (dma_cycle(snes->dma)) {}"
DMA_REPLACEMENT = "SNES_PROF_DMA_CALL(while (dma_cycle(snes->dma)) {});"

INCLUDE_LINE = '#include "snes_profile.h"   /* SNES_DEVICE_PROFILE: ledger B probes */\n'


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: instrument.py <snes.c> <outdir>\n")
        return 2
    src_path, out_dir = argv[1], argv[2]

    with open(src_path, "r", encoding="utf-8") as fh:
        src = fh.read()

    # Fail loudly rather than emitting a file that silently measures nothing.
    # A profiler that compiles but never runs its probes is the exact failure
    # the 32X device profile shipped twice (CLAUDE.md, "the bug is usually in
    # the thing that never got wired").
    n_apu = src.count(APU_ANCHOR)
    n_ppu = src.count(PPU_ANCHOR)
    n_dma = src.count(DMA_ANCHOR)
    if n_dma != 1:
        sys.stderr.write(
            "snes_prof: expected exactly 1 occurrence of the DMA anchor in %s, "
            "found %d. external/sm moved underneath this script -- fix the "
            "anchor, do not weaken the check.\n" % (src_path, n_dma))
        return 1
    if n_apu != 1:
        sys.stderr.write(
            "snes_prof: expected exactly 1 occurrence of the APU anchor in %s, "
            "found %d. external/sm moved underneath this script -- fix the "
            "anchor, do not weaken the check.\n" % (src_path, n_apu))
        return 1
    if n_ppu < 1:
        sys.stderr.write(
            "snes_prof: PPU anchor not found in %s. external/sm moved "
            "underneath this script.\n" % src_path)
        return 1

    out = src.replace(APU_ANCHOR, APU_REPLACEMENT, 1)
    out = out.replace(DMA_ANCHOR, DMA_REPLACEMENT, 1)
    # Every occurrence: the file carries two call sites behind different
    # #ifdefs and only one compiles, so instrumenting both is how the probe
    # survives a configuration change instead of quietly disappearing.
    out = out.replace(PPU_ANCHOR, PPU_REPLACEMENT)
    out = INCLUDE_LINE + out

    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, "snes.c")
    with open(dst, "w", encoding="utf-8") as fh:
        fh.write(out)

    sys.stderr.write("snes_prof: instrumented %s -> %s (apu=%d ppu=%d dma=%d)\n"
                     % (src_path, dst, n_apu, n_ppu, n_dma))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
