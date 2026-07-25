#!/bin/bash
# DSP-1 HLE device-shaped reproduction: -DTARGET_GNW (device compile-time
# reality) + -fsanitize=address,alignment on the HOST (same pattern as
# tools/sm_harness/device_run.sh, which already caught one real device
# fault this way -- a 64-bit misaligned store only ARM's LDRD/STRD traps
# on). ASan catches OOB/UAF the host would survive silently; UBSan's
# alignment check flags accesses ARM would trap that x86/aarch64 allow.
#
# Three failure modes, all first-class FAILs:
#   1. sanitizer report on stderr        (memory/alignment bug)
#   2. nonzero exit                      (functional regression assert)
#   3. TIMEOUT                           (hang -- how the cmd_inverse
#      infinite loop was actually caught: the sweep stalled at cmd 0x10
#      twice with ZERO sanitizer output. Silence + no progress is a bug
#      signature here, never an infra hiccup.)
#
# Known limitation, learned the expensive way: this harness CANNOT see
# overlay-layout bugs (the atan/.overlay_nes_fceu busfault) -- those exist
# only in the device link. Before swapping any libm call in dsp1_hle.c,
# grep STM32H7B0VBTx_SDCARD.ld for `libm_a-` capture rules.
#
# Usage: dsp1_run.sh
set -e
cd "$(dirname "$0")/../.."
OUT=${OUT:-/tmp/dsp1_device_run}
mkdir -p "$OUT" && rm -f "$OUT"/*.o

SM=external/sm
SAN="-fsanitize=address,alignment -fno-sanitize-recover=address,alignment"

gcc -c -O1 -g -DNDEBUG -DTARGET_GNW -w $SAN \
    -I"$SM/src/snes" "$SM/src/snes/dsp1_hle.c" -o "$OUT/dsp1_hle.o"
gcc -c -O1 -g -DTARGET_GNW -Werror=implicit-function-declaration $SAN \
    -I"$SM/src/snes" tools/dsp1_harness/dsp1_device_run.c -o "$OUT/main.o"
gcc $SAN -o "$OUT/dsp1_device" "$OUT/dsp1_hle.o" "$OUT/main.o" -lm
echo "  built: $OUT/dsp1_device   (device compile-time reality + ASan + UBSan-alignment)"

LOG="$OUT/san.log"
rc=0
ASAN_OPTIONS=halt_on_error=0 UBSAN_OPTIONS=halt_on_error=0,print_stacktrace=1 \
    timeout 300 "$OUT/dsp1_device" >"$OUT/stdout.log" 2>"$LOG" || rc=$?

tail -12 "$OUT/stdout.log"

if [ "$rc" -eq 124 ]; then
  echo "  FAIL: TIMEOUT (hang) -- last progress line above says where. Full log: $OUT/stdout.log"
  exit 1
fi
if grep -qE "ERROR: (Address|Leak)Sanitizer|runtime error:" "$LOG"; then
  echo "  FAIL: sanitizer report:"
  sed 's/^/    /' "$LOG"
  exit 1
fi
if [ "$rc" -ne 0 ]; then
  echo "  FAIL: harness exit $rc (functional regression -- see stdout above)"
  exit 1
fi
echo "  ok: full sweep + raster streaming + inverse regression, no sanitizer report, no hang"
