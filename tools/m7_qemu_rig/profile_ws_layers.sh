#!/bin/bash
# Attribute the WonderSwan PPU cost across BG / FG / sprite layers, on the M7 rig.
# Compiles the core ONCE, then relinks rig_wswan.o built with different RIG_LAYERS
# masks (bit0=BG bit1=FG bit2=sprite) plus a render-OFF baseline (CPU only).
#   PPU(all)  = on(7) - off      BG = on(1) - off
#   sprite    = on(7) - on(3)    FG = on(3) - on(1)
#
#   bash tools/m7_qemu_rig/profile_ws_layers.sh <rom> [frames]
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: profile_ws_layers.sh <rom> [frames]}"
FRAMES="${2:-1200}"

CW=external/oswan-go/main
RIG=tools/m7_qemu_rig
OUT="$RIG/build/wsprof"
mkdir -p "$OUT"

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=soft"
OPT="-Os -g -fno-strict-aliasing -ffunction-sections -fdata-sections"
DEF="-DGNW_WSWAN -DNOSDL_FB -DSOUND_ON -DSOUND_EMULATION -DRIG_FRAMES=$FRAMES"
INC="-ICore/Inc/porting/wswan -ICore/Src/porting/lib \
     -I$CW/emu -I$CW/emu/cpu -I$CW/headers -I$CW/sound -I."

cp "$ROM" "$OUT/rom.ws"
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm \
    --rename-section .data=.rom,alloc,load,readonly,data,contents rom.ws rom.o)

CORE="$CW/emu/WS.c Core/Src/porting/wswan/ws_fileio.c $CW/emu/WSRender.c \
      $CW/emu/WSApu.c Core/Src/porting/wswan/nec.c $RIG/rig_runtime.c"
COREOBJ=""
for s in $CORE; do
    o="$OUT/$(basename "${s%.c}").o"
    [ -f "$o" ] || $CC -c $ARCH $OPT $DEF $INC "$s" -o "$o"
    COREOBJ="$COREOBJ $o"
done

link_run() {  # render(0/1) layers(mask) tag
    local render="$1" layers="$2" tag="$3"
    $CC -c $ARCH $OPT $DEF $INC "-DRIG_RENDER=$render" "-DRIG_LAYERS=$layers" \
        "$RIG/rig_wswan.c" -o "$OUT/rig_$tag.o"
    $CC $ARCH -T "$RIG/mps2_an500_ws.ld" -nostartfiles -Wl,--gc-sections \
        $COREOBJ "$OUT/rom.o" "$OUT/rig_$tag.o" -lm -o "$OUT/rig_$tag.elf"
    # take the LAST window's run= (gameplay), not the all-frames average
    timeout 1800 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
        -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_$tag.elf" 2>&1 \
        | grep -oE '^w[0-9]+ run=[0-9]+' | tail -1 | sed 's/.*run=//'
}

echo "profiling $FRAMES frames (gameplay is the tail windows)..."
OFF=$(link_run 0 7 off)
ON7=$(link_run 1 7 on7)
ON3=$(link_run 1 3 on3)
ON1=$(link_run 1 1 on1)

echo ""
echo "================ WS PPU LAYER BREAKDOWN (avg insn/frame) ================"
printf "  CPU (render off)   : %s\n" "$OFF"
printf "  BG   layer         : %s\n" "$(( ON1 - OFF ))"
printf "  FG   layer         : %s\n" "$(( ON3 - ON1 ))"
printf "  sprite layer       : %s\n" "$(( ON7 - ON3 ))"
printf "  PPU total          : %s\n" "$(( ON7 - OFF ))"
printf "  drawn frame (on7)  : %s\n" "$ON7"
echo "  NOTE: averaged over ALL frames incl. title; gameplay PPU is higher."
echo "========================================================================"
