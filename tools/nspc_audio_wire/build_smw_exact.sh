#!/bin/bash
# Build the generic SNES emulator with SMW's exact native SPC player wired in.
# Generated copies keep both external/sm and external/smw submodules untouched.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

HERE=tools/nspc_audio_wire
OUT=/tmp/smw_exact_wire_build
mkdir -p "$OUT"
rm -f "$OUT"/*.o "$OUT"/wire_smw_host

# Use the exact generator consumed by the firmware Makefile.  The proof must
# never compile a hand-maintained approximation of the product sources.
python3 "$HERE/gen_smw_exact.py" external/smw/src/smw_spc_player.c \
  external/sm/src/snes/apu.c "$OUT"

CORE="-O2 -g -ffunction-sections -fdata-sections -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -w -Iexternal/sm -Itools/sm_harness/shim"
SMINC="-iquote external/sm/src"
OBJS=""
for f in external/sm/src/snes/*.c external/sm/src/tracing.c; do
  b="$(basename "$f")"
  [ "$b" = apu.c ] && continue
  o="$OUT/${b%.c}.o"
  gcc -c $CORE "$f" -o "$o"
  OBJS="$OBJS $o"
done
gcc -c $CORE -iquote external/sm/src/snes "$OUT/apu_wire.c" -o "$OUT/apu_wire.o"
gcc -c $CORE -Iexternal/smw/src "$OUT/smw_spc_player_gen.c" -o "$OUT/smw_spc_player_gen.o"
gcc -c $CORE $SMINC -Iexternal/smw/src -I"$HERE" -Itools/snes_survey \
  "$HERE/smw_exact_wire.c" -o "$OUT/smw_exact_wire.o"
gcc -c $CORE $SMINC -I"$HERE" "$HERE/host_main.c" -o "$OUT/host_main.o"

gcc -Wl,--gc-sections -o "$OUT/wire_smw_host" $OBJS \
  "$OUT/apu_wire.o" "$OUT/smw_spc_player_gen.o" \
  "$OUT/smw_exact_wire.o" "$OUT/host_main.o" -lm
size "$OUT/wire_smw_host"
echo "BUILD OK -> $OUT/wire_smw_host"
