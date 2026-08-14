#!/bin/bash
# Sega 32X (picodrive, GNW trimmed set) on QEMU Cortex-M7: host insn/frame +
# frames-advance gate. Compiles the SAME sources/defines as the device overlay
# (external/picodrive in-tree, -DGNW_32X_CORE -DEMU_G68K), so the Thumb
# function-pointer encoding in the memory maps is exercised for real — the
# fault class a host build can never reach.
#   bash tools/m7_qemu_rig/run_32x.sh <rom.32x> [frames]
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_32x.sh <rom.32x> [frames]}"
FRAMES="${2:-600}"

PD="${PD_DIR:-external/picodrive}"
RIG=tools/m7_qemu_rig
OUT="${RIG_OUT:-$RIG/build/32x}"   # override for parallel lanes (objects clash otherwise)
mkdir -p "$OUT"

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 -ffp-contract=off"
OPT="-O2 -g -fno-strict-aliasing -ffunction-sections -fdata-sections -fcommon"
# Device defines (cf. tools/gen_md32x_redefines.sh / Makefile MD32X) + rig counters
DEF="-DGNW_32X_CORE -DEMU_G68K -DTABLES_FULL -D_USE_CZ80 -DNDEBUG -DRIG_SH2_COUNT -DRIG_FRAMES=$FRAMES ${EXTRA_DEF:-}"
# PHASE_PROF=1: per-phase cost table (picodrive pprof probes, icount clock)
if [ "${PHASE_PROF:-0}" = "1" ]; then DEF="$DEF -DRIG_PHASE_PROF"; fi
if [ -n "${RIG_32X_SSF:-}" ]; then DEF="$DEF -DRIG_32X_SSF"; fi
# FRAME_HIST=1: per-frame host-instruction distribution (p50/p90/p95/p99 + 20 bins)
if [ "${FRAME_HIST:-0}" = "1" ]; then DEF="$DEF -DRIG_FRAME_HIST"; fi
# SH2_PC_HIST=1: SH-2 guest-PC histogram (top-50 hot PCs per core, direct/delay split)
if [ "${SH2_PC_HIST:-0}" = "1" ]; then DEF="$DEF -DRIG_SH2_PC_HIST"; fi
# POLL_PEEK=1: dump r[]/gbr at each distinct backward-branch site (resolves poll region)
if [ "${POLL_PEEK:-0}" = "1" ]; then DEF="$DEF -DRIG_POLL_PEEK"; fi
# SDRAM_POLL_DIAG=1: counters + samples for the SDRAM poll case of gnw_sh2_fastloop
if [ "${SDRAM_POLL_DIAG:-0}" = "1" ]; then DEF="$DEF -DRIG_SDRAM_POLL_DIAG"; fi
INC="-I$PD -I$PD/pico -I$PD/cpu -I$PD/zlib"

# ROM -> byteswap (mirror the device flash cache byte_swap=true: the GNW
# zero-copy path in pico/cart.c expects a PRE-BYTESWAPPED 16-bit image and
# skips picodrive's own Byteswap) -> .rom section.
python3 - "$ROM" "$OUT/rom.32x" <<'EOF'
import sys
data = bytearray(open(sys.argv[1], 'rb').read())
if len(data) % 2: data.append(0)
data[0::2], data[1::2] = data[1::2], data[0::2]
open(sys.argv[2], 'wb').write(data)
EOF
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm \
    --rename-section .data=.rom,alloc,load,readonly,data,contents rom.32x rom.o)

# Real 32X BIOS, optional: RIG_32X_BIOS=<dir with the three No-Intro blobs>.
# picodrive fakes the BIOS's effects when these are absent, which is enough for
# the retail carts and demonstrably not enough for D32XR.
BIOSOBJS=""
if [ -n "${RIG_32X_BIOS:-}" ]; then
  bm="$RIG_32X_BIOS/[BIOS] 32X M68000 (USA).bin"
  bM="$RIG_32X_BIOS/[BIOS] 32X SH-2 Master (USA).bin"
  bS="$RIG_32X_BIOS/[BIOS] 32X SH-2 Slave (USA).bin"
  for f in "$bm" "$bM" "$bS"; do
    [ -f "$f" ] || { echo "missing BIOS blob: $f" >&2; exit 2; }
  done
  cp "$bm" "$OUT/bios_m68k"; cp "$bM" "$OUT/bios_msh2"; cp "$bS" "$OUT/bios_ssh2"
  for n in bios_m68k bios_msh2 bios_ssh2; do
    (cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm \
        --rename-section .data=.rom,alloc,load,readonly,data,contents "$n" "$n.o")
    BIOSOBJS="$BIOSOBJS $OUT/$n.o"
  done
  DEF="$DEF -DRIG_32X_BIOS"
fi

# The device overlay's trimmed picodrive set (Makefile MD32X_C_SOURCES /
# gen_md32x_redefines.sh) + zlib/crc32.c standing in for the firmware crc32_le.
SRCS="
cpu/sh2/sh2.c cpu/sh2/mame/sh2pico.c cpu/sh2/mame/sh2dasm.c
cpu/gwenesis68k/m68kcpu.c cpu/gwenesis68k/g68k_bus.c cpu/cz80/cz80.c
pico/32x/32x.c pico/32x/draw.c pico/32x/memory.c pico/32x/pwm.c pico/32x/sh2soc.c
pico/cart.c pico/memory.c pico/draw.c pico/sek.c pico/videoport.c
pico/media.c pico/pico.c pico/misc.c pico/patch.c pico/z80if.c
pico/eeprom.c pico/state.c
${RIG_32X_SSF:+pico/carthw/carthw.c}
pico/sound/sound.c pico/sound/mix.c pico/sound/sn76496.c pico/sound/ym2612.c
pico/sound/resampler.c
zlib/crc32.c
"

objs="$OUT/rom.o$BIOSOBJS"
for s in $SRCS; do
    o="$OUT/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')"
    $CC -c $ARCH $OPT $DEF $INC "$PD/$s" -o "$o"
    objs="$objs $o"
done
# rig harness + runtime + the Draw2FB binding the trimmed set must provide
$CC -c $ARCH $OPT $DEF -I$PD "$RIG/rig_32x.c"         -o "$OUT/rig_32x.o"
$CC -c $ARCH $OPT $DEF $INC  "$RIG/rig_32x_draw2fb.c" -o "$OUT/rig_32x_draw2fb.o"
$CC -c $ARCH $OPT $DEF       "$RIG/rig_runtime.c"     -o "$OUT/rig_runtime.o"
objs="$objs $OUT/rig_32x.o $OUT/rig_32x_draw2fb.o $OUT/rig_runtime.o"

$CC $ARCH -T "$RIG/mps2_an500_32x.ld" -nostartfiles -Wl,--gc-sections \
    $objs -lm -lc -o "$OUT/rig_32x.elf"
arm-none-eabi-size "$OUT/rig_32x.elf"

timeout "${RIG_TIMEOUT:-1800}" qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_32x.elf" 2>&1
