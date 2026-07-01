#!/bin/bash
#
# Virtual Boy DEVICE-PATH host harness (mirrors linux/lynx/run_lynx_host_test.sh).
#
# Compiles the red-viper core (external/red-viper) with -DGNW_VB_DEVICE — the
# flash-XIP ROM masking + single-player RAM the STM32H7 firmware uses — under
# AddressSanitizer + UBSan, and exercises the exact device code paths WITHOUT
# any 3DS/SDL/GL platform layer. Tests boot, and the real Core/Src/porting/vb/
# vb_savestate.c save/load module (full round-trip + truncation + CRC guard).
#
# Any ASan/UBSan error, link failure, or test FAIL exits non-zero → fails CI,
# so most device bugs are caught on the host before anything is flashed.
#
# Bugs this harness has already caught pre-device:
#   * vb_savestate.c size field must be uint32_t (WORD) — a uint16_t truncated
#     DISPLAY_RAM(256K)+VB_RAM(64K) to 0 => savestates silently lost VRAM/WRAM.
#   * device sound_init_backend() MUST return success — sound_init() frees the
#     wave buffers on failure and sound_update() then writes freed memory (UAF).
#
# Run from the repo root:  bash linux/vb/run_vb_host_test.sh
set -euo pipefail
cd "$(dirname "$0")/../.."

RV=external/red-viper
OUT=linux/vb/build_vb
mkdir -p "$OUT"

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"
DEF="-DGNW_VB_DEVICE -DDEBUGLEVEL=0"
INC="-I$RV/include -I$RV/source/common/inih"

# Core units that build for the device (interpreter path; no GLES2/minizip need
# beyond linking). video_common/video_soft supply tDSPCACHE/soft-render without
# the GLES2 hard renderer.
CORE_C="v810_cpu v810_ins v810_mem interpreter vb_sound vb_set rom_db patches replay video_common"
for f in $CORE_C; do
  gcc $SAN $DEF $INC -c "$RV/source/common/$f.c" -o "$OUT/$f.o"
done
g++ $SAN $DEF $INC -fno-rtti -fno-exceptions -c "$RV/source/common/video_soft.cpp" -o "$OUT/video_soft.o"
gcc $SAN $DEF $INC -c "$RV/source/common/inih/ini.c" -o "$OUT/ini.o"

# Device save/load module under test + the harness driver.
gcc $SAN $DEF $INC -c Core/Src/porting/vb/vb_savestate.c -o "$OUT/vb_savestate.o"
gcc $SAN $DEF $INC -c linux/vb/vb_harness.c              -o "$OUT/vb_harness.o"

g++ $SAN "$OUT"/*.o -o "$OUT/vb_harness.elf" -lm -lminizip -lz

ASAN_OPTIONS=detect_leaks=0 "$OUT/vb_harness.elf"
