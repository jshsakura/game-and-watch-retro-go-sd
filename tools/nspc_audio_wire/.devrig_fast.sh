#!/bin/bash
# Live-wired N-SPC HLE on the hard-float M7 rig -- but linking the DEVICE's
# own copy, tools/nspc_audio_wire/nspc_wire.c (the file Makefile.common:1504
# actually compiles into the firmware), not tools/nspc_audio_wire/wire.c (the
# host proof-of-concept run_snes_wire.sh / build.sh use). The two files are
# independently written and have already diverged once in device-only ways
# (see nspc_wire.c's "FIRST SONG AFTER A SILENT SWAP" comment) -- this script
# exists so that class of bug reproduces here, on a rig, instead of first
# being found on a user's hardware. See CLAUDE.md, "Testing a core the way
# the device runs it".
#
# Self-contained: generates its own spc_player_gen.{c,h}/apu_wire.c copies
# via gen_nspc_wire.py (the SAME generator Makefile.common's SNES_NSPC_HLE=1
# path uses) rather than depending on another script having run first.
#
# Output directory: defaults to /tmp/nspc_wire_build_device -- deliberately
# NOT /tmp/nspc_wire_build, which run_snes_wire.sh / build.sh use and which
# other concurrent sessions may be writing to. Override with
# NSPC_DEVICE_WIRE_BUILD=<dir> if you need a different private location.
#
# WIRE_OFF=1 -> stock LLE reference (wire disabled at runtime, same binary).
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_snes_device_wire.sh <rom.smc> [frames]}"
FRAMES="${2:-1500}"

if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
  echo "[run_snes_device_wire] SKIP: arm-none-eabi-gcc not found on PATH"
  exit 0
fi
if ! command -v qemu-system-arm >/dev/null 2>&1; then
  echo "[run_snes_device_wire] SKIP: qemu-system-arm not found on PATH"
  exit 0
fi

SM=external/sm
RIG=tools/m7_qemu_rig
HERE=tools/nspc_audio_wire
HLE=tools/nspc_hle
GEN="${NSPC_DEVICE_WIRE_BUILD:-/tmp/nspc_wire_build_device}"
NGEN="$GEN/nspc_hle_gen"
OUT="$GEN/rig"
mkdir -p "$NGEN" "$OUT"

# ---- 1) generate the device's own spc_player_gen.{c,h} + apu_wire.c -------
# Exactly what Makefile.common's SNES_NSPC_HLE=1 rule runs (see
# $(SNES_NSPC_HLE_DIR)/.stamp), so this rig links the identical generated
# sources the firmware does -- not a hand-copy that can drift from them.
python3 "$HERE/gen_nspc_wire.py" \
  "$SM/src/spc_player.c" "$SM/src/spc_player.h" "$SM/src/snes/apu.c" "$NGEN"

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"
OPT="-O2 -g -ffunction-sections -fdata-sections -ffp-contract=off"
DEF="-DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -DRIG_FRAMES=$FRAMES -DRIG_WINDOW=${RIG_WINDOW:-200} ${RIG_EXTRA_DEF:-}"
if [ "${WIRE_OFF:-0}" = "1" ]; then DEF="$DEF -DWIRE_OFF=1"; fi
INC="-I$SM -I$RIG/shim -Itools/sm_harness/shim"
SMINC="-iquote $SM/src"
# spc_player_gen.h / nspc_config.h / snes_driver_sigs.h / wire.h are all
# unqualified #includes -- same include set Makefile.common's SNES_HLE_INCLUDES
# gives the firmware build for SNES_NSPC_HLE=1.
NSPCINC="-I$NGEN -I$HLE -Itools/snes_survey -I$HERE"

cp "$ROM" "$OUT/rom.smc"
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm --rename-section .data=.rom_blob,alloc,load,readonly,data,contents rom.smc rom.o)

SRCS="$SM/src/snes/cart.c $SM/src/snes/cpu.c $SM/src/snes/cx4_hle.c \
      $SM/src/snes/dma.c $SM/src/snes/dsp.c \
      $SM/src/snes/input.c $SM/src/snes/ppu.c $SM/src/snes/snes.c \
      $SM/src/snes/snes_other.c $SM/src/snes/spc.c $SM/src/snes/rc_dispatch.c \
      $SM/src/tracing.c $RIG/rig_runtime_hf.c"

OBJS=""
for s in $SRCS; do
    o="$OUT/$(basename "${s%.c}").o"
    if [ -n "${REUSE_OBJS:-}" ] && [ -f "$o" ]; then :; else $CC -c $ARCH $OPT $DEF $INC -w "$s" -o "$o"; fi
    OBJS="$OBJS $o"
done
# apu_wire.c: apu.c with apu_run renamed apu_run_lle (device's own generated copy).
if [ -n "${REUSE_OBJS:-}" ] && [ -f "$OUT/apu_wire.o" ]; then :; else $CC -c $ARCH $OPT $DEF $INC -iquote $SM/src/snes -w "$NGEN/apu_wire.c" -o "$OUT/apu_wire.o"; fi
# spc_player_gen.c: zero-copy, dialect-parameterized player (device's own copy).
if [ -n "${REUSE_OBJS:-}" ] && [ -f "$OUT/spc_player_gen.o" ]; then :; else $CC -c $ARCH $OPT $DEF $INC $SMINC -include "$HLE/nspc_config.h" -w "$NGEN/spc_player_gen.c" -o "$OUT/spc_player_gen.o"; fi
if [ -n "${REUSE_OBJS:-}" ] && [ -f "$OUT/nspc_variant.o" ]; then :; else $CC -c $ARCH $OPT $DEF $INC $SMINC -I"$HLE" -w "$HLE/nspc_variant.c" -o "$OUT/nspc_variant.o"; fi
# THE FILE UNDER TEST: the firmware's own tools/nspc_audio_wire/nspc_wire.c,
# never edited by this script.
if [ -n "${REUSE_OBJS:-}" ] && [ -f "$OUT/nspc_wire.o" ]; then :; else $CC -c $ARCH $OPT $DEF $INC $SMINC $NSPCINC -w "$HERE/nspc_wire.c" -o "$OUT/nspc_wire.o"; fi
if [ -n "${REUSE_OBJS:-}" ] && [ -f "$OUT/rig_snes_device_wire.o" ]; then :; else $CC -c $ARCH $OPT $DEF $INC -w "$HERE/rig_snes_device_wire.c" -o "$OUT/rig_snes_device_wire.o"; fi
OBJS="$OBJS $OUT/apu_wire.o $OUT/spc_player_gen.o $OUT/nspc_variant.o $OUT/nspc_wire.o $OUT/rig_snes_device_wire.o $OUT/rom.o"

$CC $ARCH -T "$RIG/mps2_an500_snes.ld" -nostartfiles -Wl,--gc-sections \
    $OBJS -lm -o "$OUT/rig_snes_device_wire.elf"

arm-none-eabi-size "$OUT/rig_snes_device_wire.elf"

timeout 1800 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_snes_device_wire.elf"
