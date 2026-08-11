#!/usr/bin/env bash
# Drawn frames per second -- the metric bench.sh cannot see.
#
# The overload guard (Core/Src/porting/common.c) draws one frame in four under
# sustained load, so bench.sh's fps counts EMULATED frames and the player sees a
# quarter of them: 57.92 emulated against 14.85 drawn, ratio 0.256, measured.
#
# The consequence is not obvious and cost a shelved optimisation: making a
# SKIPPED frame cheaper lets the guard draw more, which raises what the player
# sees and LOWERS the fps number. SNES_SPRITE_SKIP_DRAW was dropped a session
# ago as "nothing, 52.29 vs 52.36" for exactly that reason.
#
#   drawrate.sh <arm> [seconds]
#
# Reads g_snes_drawn_frames and snes->frames over SWD twice and reports both
# rates and the ratio. Use this for anything on the frameskip path; use
# bench.sh for anything on the drawn path.
set -eu
ARM=${1:?usage: drawrate.sh <arm> [seconds]}
SECS=${2:-20}
HOST=${PROBE_HOST:-rpi-genie5}
E=/tmp/gnw_arms/$ARM/gw_retro_go.elf
OC="sudo openocd -f interface/stlink-dap.cfg -f target/stm32h7x.cfg -c 'adapter speed 4000'"
sym() { arm-none-eabi-nm "$E" | awk -v s="$1" '$3==s{print "0x"$1; exit}'; }
rd() { ssh "$HOST" "$OC -c init -c 'mdw $1' -c shutdown 2>&1" | grep -oE ": [0-9a-f]{8}" | tr -d ': '; }

A=$(sym g_snes_drawn_frames)
S=$(sym snes)
P=$(rd "$S")
case "$P" in 20*|24*|30*) ;; *) echo "no SNES ROM running (snes=0x$P)"; exit 1;; esac
F=$(printf '0x%x' $((0x$P + 56)))    # offsetof(Snes, frames)

d0=$((16#$(rd "$A"))); f0=$((16#$(rd "$F"))); t0=$(date +%s.%N)
sleep "$SECS"
d1=$((16#$(rd "$A"))); f1=$((16#$(rd "$F"))); t1=$(date +%s.%N)
python3 -c "
dt=$t1-$t0
print(f'$ARM: drawn {($d1-$d0)/dt:.2f} fps   emulated {($f1-$f0)/dt:.2f} fps   draw ratio {($d1-$d0)/max(1,$f1-$f0):.3f}')"
