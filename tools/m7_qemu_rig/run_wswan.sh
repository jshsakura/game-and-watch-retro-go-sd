#!/bin/bash
# WonderSwan (oswan) on QEMU's Cortex-M7 (mps2-an500): executed-instruction
# counts per frame, split CPU / PPU / blit, on a real ARMv7-M instruction stream.
#
#   bash tools/m7_qemu_rig/run_wswan.sh <rom.ws|.wsc> [frames]
#
# Builds the rig TWICE from the device's own WSWAN source list and defines:
#   RIG_RENDER=1  -> WsRun = CPU emulation + PPU per-scanline render
#   RIG_RENDER=0  -> WsRun = CPU emulation only (render skipped; same CPU state)
# so PPU cost = (render-on run) - (render-off run). Prints a combined ledger.
#
# -icount shift=0 makes virtual time tick 1 ns per executed instruction; the
# board's CMSDK timer runs on virtual time, so timer deltas are instruction
# counts (the rig self-calibrates the scale at boot). NOT modelled: caches and
# wait states (QEMU has neither) — absolute device fps still comes from the
# device. FrameBuffer hashes match a host build's for the same ROM + script.
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_wswan.sh <rom.ws|.wsc> [frames]}"
FRAMES="${2:-3000}"

CW=external/oswan-go/main
RIG=tools/m7_qemu_rig
OUT="$RIG/build/ws"
mkdir -p "$OUT"

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=soft"
OPT="-Os -g -fno-strict-aliasing -ffunction-sections -fdata-sections"
DEF="-DGNW_WSWAN -DNOSDL_FB -DSOUND_ON -DSOUND_EMULATION -DRIG_FRAMES=$FRAMES"
INC="-ICore/Inc/porting/wswan -ICore/Src/porting/lib \
     -I$CW/emu -I$CW/emu/cpu -I$CW/headers -I$CW/sound -I."

# ROM -> object (symbols _binary_rom_ws_start/end). Rename the generated .data
# section to .rom so the linker script can XIP it in the 16MB PSRAM region
# rather than copying an 8MB blob into 4MB RAM.
cp "$ROM" "$OUT/rom.ws"
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm \
    --rename-section .data=.rom,alloc,load,readonly,data,contents rom.ws rom.o)

# device WSWAN_C_SOURCES minus main_wswan.c (rig_wswan.c replaces it)
SRCS_C="$CW/emu/WS.c Core/Src/porting/wswan/ws_fileio.c \
        $CW/emu/WSRender.c $CW/emu/WSApu.c Core/Src/porting/wswan/nec.c \
        $RIG/rig_runtime.c"

build_variant() {
    local render="$1" tag="$2"
    local objs="$OUT/rom.o"
    for s in $SRCS_C; do
        local o="$OUT/${tag}_$(basename "${s%.c}").o"
        $CC -c $ARCH $OPT $DEF $INC "$s" -o "$o"
        objs="$objs $o"
    done
    $CC -c $ARCH $OPT $DEF $INC "-DRIG_RENDER=$render" "$RIG/rig_wswan.c" \
        -o "$OUT/${tag}_rig_wswan.o"
    objs="$objs $OUT/${tag}_rig_wswan.o"
    $CC $ARCH -T "$RIG/mps2_an500_ws.ld" -nostartfiles -Wl,--gc-sections \
        $objs -lm -o "$OUT/rig_ws_${tag}.elf"
}

echo "=== building render-ON variant ==="
build_variant 1 on
echo "=== building render-OFF variant ==="
build_variant 0 off
arm-none-eabi-size "$OUT/rig_ws_on.elf"

run_variant() {
    # QEMU semihosting SYS_WRITE0 goes to stderr; fold it into stdout so tee sees it.
    timeout 1800 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
        -icount shift=0,align=off,sleep=off -kernel "$1" 2>&1
}

echo "=== RENDER ON (emu + ppu) ==="
run_variant "$OUT/rig_ws_on.elf"  | tee "$OUT/out_on.txt"
echo "=== RENDER OFF (emu only) ==="
run_variant "$OUT/rig_ws_off.elf" | tee "$OUT/out_off.txt"

# combined ledger: emu = off-run "run", ppu = on-run "run" - off-run "run"
awk_line() { grep '^\[ws-qemu\] done' "$1" | sed 's/.*avg run=\([0-9]*\) blit=\([0-9]*\).*/\1 \2/'; }
read -r RUN_ON  BLIT_ON  <<<"$(awk_line "$OUT/out_on.txt")"
read -r RUN_OFF BLIT_OFF <<<"$(awk_line "$OUT/out_off.txt")"
HASH_ON=$(grep '^\[ws-qemu\] done' "$OUT/out_on.txt" | sed 's/.*RUNHASH=\([0-9a-f]*\).*/\1/')
PPU=$(( RUN_ON - RUN_OFF ))
echo ""
echo "================ WS M7 LEDGER (avg insn/frame, $FRAMES frames) ================"
echo "  CPU (emu)   : $RUN_OFF"
echo "  PPU (render): $PPU"
echo "  blit (scale): $BLIT_ON"
echo "  --------------------------------"
echo "  drawn frame : $(( RUN_ON + BLIT_ON ))   (emu+ppu+blit)"
echo "  RUNHASH(on) : $HASH_ON"
echo "  budget@280MHz: 3733333/frame @75fps  |  @312MHz: 4160000/frame"
echo "==============================================================================="
