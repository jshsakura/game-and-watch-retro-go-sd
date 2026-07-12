#!/bin/bash
# Run the sm core on the host with the DEVICE's compile-time reality:
#   -DTARGET_GNW  -> snes->apu is NULL (spc_player is the sound chip), the PPU
#                    renders RGB565, VRAM comes from itc_calloc.
#   the device's SM_C_SOURCES, and nothing else.
#
# The old harness compiled the whole tree without TARGET_GNW, so it had a real
# SPC700 and a real g_snes. It ran 4,000 clean frames while the device HardFaulted
# inside a 64 KB memcpy from snes->apu->ram — with snes->apu NULL. Same program, or
# it proves nothing.
set -e
cd "$(dirname "$0")/../.."
OUT=${OUT:-/tmp/sm_device_run}
mkdir -p "$OUT" && rm -f "$OUT"/*.o

SRCS=$(make -pn 2>/dev/null | grep '^SM_C_SOURCES = ' | head -1 |
       sed 's/^SM_C_SOURCES = //; s|$(CORE_SM)|external/sm|g' | tr ' ' '\n' |
       grep '\.c$' | grep -v 'main_sm\.c$')

for f in $SRCS; do
  gcc -c -O1 -g -DNDEBUG -DTARGET_GNW -DHEADLESS -w \
      -Iexternal/sm -Itools/sm_harness/shim "$f" -o "$OUT/$(basename "${f%.c}").o"
done
gcc -c -O1 -g -DTARGET_GNW -w -Iexternal/sm -Itools/sm_harness/shim \
    tools/sm_harness/device_main.c -o "$OUT/main.o"
gcc -o "$OUT/sm_device" "$OUT"/*.o -lm
echo "  built: $OUT/sm_device   (device compile-time reality)"
