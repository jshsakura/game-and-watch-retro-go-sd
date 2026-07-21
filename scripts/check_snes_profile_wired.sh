#!/usr/bin/env bash
# Prove the SNES device profiler's probes are in the BINARY, not just in the
# source tree.
#
# This is not paranoia, it is the specific failure the 32X device profile
# already shipped: a build whose profiler define never reached the objects, so
# the call sites were never compiled, the dump came out shaped correctly, and
# nobody could tell from reading it that it measured nothing. An adversarial
# review of this profiler's design listed "nm/map/disassembly gate that the
# profiler binary actually contains the instrumentation" as a precondition for
# trusting any run. This is that gate.
#
# It checks, in the objects the linker actually consumed:
#   - snes_profile.o exists and defines the recorder
#   - main_snes.o calls it            -> Ledger A is wired
#   - snes.o touches the ledger-B accumulators -> the generated instrumented
#     copy of external/sm's snes.c is what got compiled, not the pristine one
#   - the resident IRQ counters exist -> Ledger A's IRQ bound is real
#
# SKIP, LOUDLY, RATHER THAN FAIL, when the toolchain cannot answer: twice in
# one day this tree broke its own build with a safety net (the cross-overlay
# symbol check when nm was missing, the JPEG runner on a shallow clone). A net
# that breaks the build teaches people to disable it. Only a genuinely wrong
# answer fails here.
#
# usage: check_snes_profile_wired.sh <build_dir>   (no-op unless the build dir
#        actually contains a profiler object, so it is safe to run always)
set -u

BUILD_DIR="${1:-build}"
NM="${NM:-arm-none-eabi-nm}"

PROF_O="$BUILD_DIR/snes/snes_profile.o"

# Not a profiler build: nothing to check, and that is the normal case.
[ -f "$PROF_O" ] || exit 0

if ! command -v "$NM" >/dev/null 2>&1; then
  echo "check_snes_profile_wired: SKIP (no $NM on PATH) -- profiler probe" \
       "presence NOT verified for this build" >&2
  exit 0
fi

fail=0
note() { echo "check_snes_profile_wired: $*" >&2; }

# $1 object, $2 symbol, $3 'defined'|'referenced', $4 human description
want() {
  local obj="$1" sym="$2" kind="$3" desc="$4"
  if [ ! -f "$obj" ]; then
    note "SKIP: $obj not built"
    return
  fi
  local line
  line=$("$NM" "$obj" 2>/dev/null | grep -E "[ ]${sym}\$" | head -1)
  if [ -z "$line" ]; then
    note "FAIL: $desc -- $sym not present in $(basename "$obj")"
    fail=1
    return
  fi
  case "$kind" in
    defined)
      case "$line" in
        *" U "*) note "FAIL: $desc -- $sym is undefined in $(basename "$obj")"
                 fail=1 ;;
      esac
      ;;
    referenced)
      # Either an undefined reference or a local use is fine; absence is not.
      ;;
  esac
}

want "$PROF_O" "gsnes__snes_profile_record" defined \
     "the recorder is compiled"
want "$BUILD_DIR/snes/main_snes.o" "gsnes__snes_profile_record" referenced \
     "Ledger A is wired into the frame loop"
want "$BUILD_DIR/snes/main_snes.o" "gsnes__snes_prof_mark" referenced \
     "the Ledger A marks are compiled into the frame loop"
want "$BUILD_DIR/snes/snes.o" "gsnes__snes_prof_b_apu_cyc" referenced \
     "Ledger B APU scope reached the compiled snes.c (generated copy in use)"
want "$BUILD_DIR/snes/snes.o" "gsnes__snes_prof_b_ppu_cyc" referenced \
     "Ledger B PPU scope reached the compiled snes.c (generated copy in use)"
# Resident objects build into $(BUILD_DIR)/core/ (they compile -Os, unlike the
# overlay cores) -- that is the whole point of this check: the counters must NOT
# be in build/snes/.
want "$BUILD_DIR/core/stm32h7xx_it.o" "snes_prof_irq_cycles" defined \
     "the IRQ ledger's counters are in resident memory"
want "$BUILD_DIR/core/gw_audio.o" "snes_prof_irq_cycles" referenced \
     "the SAI callbacks charge into the IRQ ledger"

if [ "$fail" -ne 0 ]; then
  echo "check_snes_profile_wired: the profiler is NOT fully wired into this" \
       "binary. Any /snes_dwt.txt it produces is not evidence." >&2
  exit 1
fi

echo "check_snes_profile_wired: OK (Ledger A/B probes and IRQ counters present)" >&2
exit 0
