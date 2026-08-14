#!/usr/bin/env bash
# Photograph the console's screen over SWD, without symbols and without eyes.
#
#   screenshot.sh [out.png]
#
# Why this exists. Every measurement this repo takes on hardware is a number --
# drawn fps, emulated fps, a PC histogram -- and not one of them can tell you
# whether the panel shows the game or a black rectangle. drawn_ab.sh counts
# `lcd_swap()` calls; a core that swaps two empty buffers 60 times a second
# reads as 60 fps. So the last step of every device session has been "hold the
# console up and look at it", which needs a human in the room, cannot be put in
# a log, and was the one open question left at the end of the 32X day: the
# counters said D32XR ran at 41.4 fps and nothing knew whether Doom was on the
# screen. This answers that in nine seconds, from anywhere.
#
# HOW IT FINDS THE FRAMEBUFFER: it asks the LTDC, not the ELF.
#
# The obvious implementation resolves `framebuffer1`/`active_framebuffer` out of
# gw_retro_go.elf and reads there. Do not do that. Those are POINTERS that
# lcd_setup_framebuffers() repoints at runtime (LUT8 mode moves them and frees
# the upper half as PICO-8's bonus pool), the ELF on disk is routinely not the
# image on the device -- the failure drawn_ab.sh grew a 60-line guard against --
# and a RAM address resolved through the wrong arm's ELF lands in whichever
# overlay the linker saw first.
#
# LTDC's layer-1 registers have none of those problems. L1CFBAR is the address
# the display controller is DMAing to the panel right now: not what the firmware
# believes, not what the map file says, but the bytes actually becoming light.
# It is correct across every LCD mode, every core, every arm, and it needs no
# build artifact at all.
#
#   0x50001018  LTDC_GCR      bit0 LTDCEN -- is the controller even on
#   0x50001094  LTDC_L1PFCR   pixel format: 2 = RGB565, 5 = L8
#   0x500010AC  LTDC_L1CFBAR  scanout base address  <-- the whole trick
#   0x500010B0  LTDC_L1CFBLR  [28:16] pitch in bytes
#   0x500010B4  LTDC_L1CFBLNR line count
#
# IT DOES NOT HALT THE CPU. The read goes through the debug AP while the core
# keeps running, which is not just politeness: the emulator is drawing into the
# INACTIVE buffer while LTDC scans the active one, so a running dump is a clean
# read of a finished frame. Halting would freeze audio DMA and prove nothing
# extra. The one thing it can catch is a page flip landing mid-dump -- take two
# and compare if a frame looks torn.
#
# TWO DUMPS ARE THE LIVENESS TEST. One PNG proves the panel shows something;
# only a second one, different from the first, proves the console is still
# computing. A crashed core leaves a perfectly good last frame on screen for
# ever, and that is exactly what a single screenshot cannot distinguish from a
# healthy game. `--live` takes both and says which it is.
set -euo pipefail

HOST=${PROBE_HOST:-rpi-genie5}
IFACE=${IFACE:-interface/stlink-dap.cfg}
TARGET=${TARGET:-target/stm32h7x.cfg}
OC="sudo openocd -f $IFACE -f $TARGET -c 'adapter speed 4000'"
HERE=$(cd "$(dirname "$0")" && pwd)
CONV="$HERE/../binary_rgb565_to_png.py"

LTDC_GCR=0x50001018
L1PFCR=0x50001094
L1CFBAR=0x500010ac
L1CFBLR=0x500010b0
L1CFBLNR=0x500010b4

LIVE=0
OUT=""
for a in "$@"; do
  case "$a" in
    --live) LIVE=1 ;;
    -*)     echo "unknown option: $a" >&2; exit 2 ;;
    *)      OUT=$a ;;
  esac
done
OUT=${OUT:-gnw_screen.png}
case "$OUT" in *.png) ;; *) OUT="$OUT.png" ;; esac

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Read the five registers in one OpenOCD session. Separate sessions cost a USB
# connect each (~2 s) and, worse, can straddle a mode change.
regs=$(ssh -n "$HOST" "$OC -c init \
        -c 'mdw $LTDC_GCR' -c 'mdw $L1PFCR' -c 'mdw $L1CFBAR' \
        -c 'mdw $L1CFBLR' -c 'mdw $L1CFBLNR' -c shutdown 2>&1" \
       | sed -n 's/^0x[0-9a-f]*: \([0-9a-f]*\).*/\1/p')
mapfile -t R <<<"$regs"
[ "${#R[@]}" -ge 5 ] || { echo "could not read LTDC registers (is the probe attached?)" >&2
                          printf '%s\n' "$regs" | tail -5 >&2; exit 1; }

gcr=$((16#${R[0]})); fmt=$((16#${R[1]})); base=$((16#${R[2]}))
pitch=$(( (16#${R[3]} >> 16) & 0x1fff )); lines=$(( 16#${R[4]} & 0x7ff ))

if [ $((gcr & 1)) -eq 0 ]; then
  echo "LTDC is DISABLED -- the panel is not being driven at all." >&2
  echo "That is a real finding, not a tool failure: the console is asleep," >&2
  echo "in a fault handler that never re-enabled it, or dead before lcd_init()." >&2
  exit 3
fi
if [ "$fmt" -ne 2 ]; then
  echo "pixel format $fmt is not RGB565 (2)." >&2
  [ "$fmt" -eq 5 ] && echo "This is LUT8 mode. The palette lives in LTDC's write-only CLUT, so the" >&2 &&
                      echo "raw dump cannot be coloured from the chip -- read the core's own CLUT." >&2
  exit 4
fi
bytes=$((pitch * lines))
[ "$bytes" -gt 0 ] || { echo "LTDC reports a zero-sized frame ($pitch x $lines)" >&2; exit 5; }
width=$((pitch / 2))
printf '[screen] %dx%d RGB565 @ 0x%08x (%d bytes)\n' "$width" "$lines" "$base" "$bytes" >&2

grab() {   # grab <local.bin> -- one frame, from wherever LTDC is scanning NOW
  local dst=$1 remote="/tmp/gnw_fb.$$.$RANDOM.bin"
  ssh -n "$HOST" "$OC -c init -c 'dump_image $remote $(printf '0x%08x' "$base") $bytes' \
                     -c shutdown >/dev/null 2>&1; sudo chmod 644 $remote"
  scp -q "$HOST:$remote" "$dst"
  ssh -n "$HOST" "sudo rm -f $remote" >/dev/null 2>&1 || true
}

grab "$TMP/a.bin"
python3 "$CONV" "$TMP/a.bin" "$width" "$lines" >/dev/null
mv "$TMP/a.png" "$OUT"
echo "[screen] $OUT"

if [ "$LIVE" = 1 ]; then
  # Re-read L1CFBAR: a flip since the first grab already proves motion, and it
  # tells us which buffer to photograph so the second PNG is a different frame
  # rather than a re-read of the same one.
  b2=$(ssh -n "$HOST" "$OC -c init -c 'mdw $L1CFBAR' -c shutdown 2>&1" \
       | sed -n 's/^0x[0-9a-f]*: \([0-9a-f]*\).*/\1/p' | head -1)
  base=$((16#$b2))
  out2="${OUT%.png}_2.png"
  grab "$TMP/b.bin"
  python3 "$CONV" "$TMP/b.bin" "$width" "$lines" >/dev/null
  mv "$TMP/b.png" "$out2"
  echo "[screen] $out2"
  if cmp -s "$TMP/a.bin" "$TMP/b.bin"; then
    echo "[screen] STILL: two frames are byte-identical."
    echo "         A static menu looks like this and so does a hung core."
    echo "         Read the frame counters (drawn_ab.sh) before concluding either."
    exit 6
  fi
  # `cmp -l` exits 1 precisely when the frames DIFFER -- the success case here.
  # Under `set -euo pipefail` that killed the script one line before it printed
  # its verdict, so the tool reported failure on a healthy console. Swallow it.
  n=$({ cmp -l "$TMP/a.bin" "$TMP/b.bin" || true; } | wc -l)
  echo "[screen] LIVE: $n of $bytes bytes changed between the two frames."
fi
