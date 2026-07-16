#!/bin/bash
# 65816->C static translator PoC: dump -> translate -> baseline vs hybrid.
#   bash tools/sfc_recomp/build.sh "<rom.smc>" [frames]
# Gate: state hash AND audio hash bit-identical between the pure-interpreter
# and hybrid builds; then ms/frame ratio + runtime native coverage.
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: build.sh <rom.smc> [frames]}"
FRAMES="${2:-1500}"
DUMP_FRAMES=2000

HERE=tools/sfc_recomp
O="${SFC_RECOMP_OUT:-/tmp/sfc_recomp_build}"
mkdir -p "$O"

CF="-O2 -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -w \
    -Iexternal/sm -Itools/sm_harness/shim"
CORE_SRCS="external/sm/src/snes/apu.c external/sm/src/snes/cart.c \
  external/sm/src/snes/dma.c external/sm/src/snes/dsp.c \
  external/sm/src/snes/input.c external/sm/src/snes/ppu.c \
  external/sm/src/snes/snes.c external/sm/src/snes/snes_other.c \
  external/sm/src/snes/spc.c external/sm/src/tracing.c"

echo "== [1/5] core objects" >&2
for f in $CORE_SRCS; do
  o="$O/$(basename "${f%.c}").o"
  [ "$o" -nt "$f" ] || gcc -c $CF "$f" -o "$o"
done

echo "== [2/5] dump run ($DUMP_FRAMES frames, histogram build)" >&2
gcc -c $CF -DSNES_PC_HISTOGRAM external/sm/src/snes/cpu.c -o "$O/cpu_hist.o"
gcc -c $CF -DSNES_PC_HISTOGRAM "$HERE/harness_main.c" -o "$O/harness_hist.o"
gcc -o "$O/dump" "$O"/{apu,cart,dma,dsp,input,ppu,snes,snes_other,spc,tracing}.o \
    "$O/cpu_hist.o" "$O/harness_hist.o" -lm
SNES_SITEDUMP="$O/sites.bin" SNES_CARTDUMP="$O/cart.bin" \
  "$O/dump" "$ROM" "$DUMP_FRAMES" > "$O/dump.log" 2> "$O/dump.err" || {
    cat "$O/dump.err" >&2; exit 1; }
grep "\[sitedump\]\|\[cartdump\]" "$O/dump.err" >&2
CART_TYPE=$(sed -n 's/.*type=\([0-9]*\).*/\1/p' "$O/dump.err" | head -1)
CART_MASK=$(sed -n 's/.*romMask=\(0x[0-9a-f]*\).*/\1/p' "$O/dump.err" | head -1)

echo "== [3/5] translate (type=$CART_TYPE mask=$CART_MASK)" >&2
python3 "$HERE/translate.py" external/sm/src/snes/cpu.c \
  "$O/sites.bin" "$O/cart.bin" "$CART_TYPE" "$CART_MASK" "$O/rc_sites.inc"

echo "== [4/5] baseline + hybrid builds" >&2
gcc -c $CF external/sm/src/snes/cpu.c -o "$O/cpu.o"
gcc -c $CF "$HERE/harness_main.c" -o "$O/harness.o"
gcc -o "$O/baseline" "$O"/{apu,cart,dma,dsp,input,ppu,snes,snes_other,spc,tracing}.o \
    "$O/cpu.o" "$O/harness.o" -lm

sed 's/^int cpu_runOpcode(Cpu\* cpu) {/static int rc_orig_runOpcode(Cpu* cpu) {/' \
    external/sm/src/snes/cpu.c > "$O/cpu_copy.c"
grep -q "rc_orig_runOpcode" "$O/cpu_copy.c" || { echo "rename failed" >&2; exit 1; }
gcc -c $CF -I"$O" -Iexternal/sm/src/snes "$HERE/rc_core.c" -o "$O/rc_core.o"
gcc -c $CF -DRC_HYBRID "$HERE/harness_main.c" -o "$O/harness_hy.o"
gcc -o "$O/hybrid" "$O"/{apu,cart,dma,dsp,input,ppu,snes,snes_other,spc,tracing}.o \
    "$O/rc_core.o" "$O/harness_hy.o" -lm

echo "== [5/5] gate + measure ($FRAMES frames)" >&2
"$O/baseline" "$ROM" "$FRAMES" > "$O/base.out" 2>&1
"$O/hybrid"   "$ROM" "$FRAMES" > "$O/hyb.out"  2> "$O/hyb.err"
echo "--- baseline: $(grep -o 'state=[0-9a-f]* *audio=[0-9a-f]*.*' "$O/base.out")" >&2
echo "--- hybrid:   $(grep -o 'state=[0-9a-f]* *audio=[0-9a-f]*.*' "$O/hyb.out")" >&2
grep "\[rc\]" "$O/hyb.err" >&2 || true

BH=$(grep -o 'state=[0-9a-f]*' "$O/base.out"); HH=$(grep -o 'state=[0-9a-f]*' "$O/hyb.out")
BA=$(grep -o 'audio=[0-9a-f]*' "$O/base.out"); HA=$(grep -o 'audio=[0-9a-f]*' "$O/hyb.out")
if [ "$BH" = "$HH" ] && [ "$BA" = "$HA" ]; then
  echo "GATE PASS: state+audio hashes bit-identical" >&2
else
  echo "GATE FAIL: hashes differ (state $BH vs $HH; audio $BA vs $HA)" >&2
  exit 1
fi
echo "baseline: $(grep -o '[0-9.]* ms/frame' "$O/base.out")" >&2
echo "hybrid:   $(grep -o '[0-9.]* ms/frame' "$O/hyb.out")" >&2
