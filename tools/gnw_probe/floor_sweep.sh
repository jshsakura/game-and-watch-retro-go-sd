#!/usr/bin/env bash
# Which cores are pinned on the overload guard's floor?
#
#   floor_sweep.sh <arm> <dir:rom> [dir:rom ...]
#
# The guard forces one drawn frame in N whenever the pacing integrator is
# pinned, and until 2026-08-14 N was 4 for every core in the tree. On a core
# that cannot reach real time the integrator is ALWAYS pinned, so N is not a
# safety floor, it is the draw rate -- and the 32X was running three quarters of
# the player's frames into the bin for a saving of 1.9% of its frame.
#
# That question is now one command per system, because
#   - g_common_drawn_frames / g_common_emu_frames count every core, and
#   - the bench file takes "<dirname>:<index|name>", so any system autoboots.
#
# Read the ratio column:
#   ~1.00  the core draws what it emulates. Nothing here.
#   ~0.25  pinned on the floor. Ask what drawing actually costs on this core; if
#          it is a small share of the frame, the floor is throwing frames away.
#   between  the guard is engaged part of the time -- the core is near the line.
#
# What the ratio does NOT say is whether raising it is free. That is the trade:
# drawing more costs emulated fps, and emulated fps feeds the audio. SNES pays
# 17.65 ms to draw a frame and underruns at 1-in-3; 32X pays 1.9% and gives up
# 8% to draw everything. Measure the core's draw cost before changing its ratio.
set -eu
ARM=${1:?usage: floor_sweep.sh <arm> <dir:rom> [dir:rom ...]}
shift
FRAMES=${FRAMES:-600}
cd "$(dirname "$0")/../.."

printf '%-34s %10s %10s %7s\n' "system:rom" "emulated" "drawn" "ratio"
for sel in "$@"; do
  bash tools/gnw_probe/arm32x.sh pick "$sel" >/dev/null 2>&1
  # Two samples. One is not a measurement: the same binary read 22.01 and 19.14
  # drawn fps on the same cartridge earlier today.
  for i in 1 2; do
    line=$(bash tools/gnw_probe/drawn_ab.sh "$ARM" "$FRAMES" 2>&1 | tail -1)
    emu=$(echo "$line"   | grep -oE '\(([0-9.]+) emulated' | grep -oE '[0-9.]+')
    drawn=$(echo "$line" | grep -oE '([0-9.]+) drawn'      | grep -oE '[0-9.]+')
    ratio=$(echo "$line" | grep -oE 'ratio=[0-9.]+'        | cut -d= -f2)
    printf '%-34s %10s %10s %7s\n' "${sel:0:34}" "${emu:-?}" "${drawn:-?}" "${ratio:-?}"
  done
done
