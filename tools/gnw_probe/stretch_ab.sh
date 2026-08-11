#!/usr/bin/env bash
# Frame-aligned stretcher A/B.
#
# The counters accumulate from boot and a wall-clock window is not the same
# window twice: the same default build read 0 and 181 underruns on two
# thirty-second samples, which is wider than most of the differences being
# judged. The scene is deterministic from the savestate, so take the DELTA
# across a fixed number of EMULATED frames instead and every arm sees exactly
# the same audio.
set -eu
ARM=$1; FRAMES=${2:-1800}; HOST=${PROBE_HOST:-rpi-genie5}
E=/tmp/gnw_arms/$ARM/gw_retro_go.elf
OC="sudo openocd -f interface/stlink-dap.cfg -f target/stm32h7x.cfg -c 'adapter speed 4000'"
sym() { arm-none-eabi-nm "$E" | awk -v s="$1" '$3==s{print "0x"$1; exit}'; }
SNESP=$(sym snes)
NAMES="g_stretch_ins g_stretch_pulls g_stretch_noise_pulls g_stretch_rev underruns"
ADDRS=""; for n in $NAMES; do a=$(sym $n); [ -z "$a" ] && a=$(sym g_stretch_ins); ADDRS="$ADDRS $a"; done

read_all() {   # frames + counters in one openocd session
  local cmd="-c 'mdw $SNESP'"
  for a in $ADDRS; do cmd="$cmd -c 'mdw $a'"; done
  eval ssh "$HOST" "\"$OC -c init $cmd -c shutdown 2>&1\"" | grep -oE ": [0-9a-f]{8}" | tr -d ': '
}
frames_of() { ssh "$HOST" "$OC -c init -c 'mdw $1' -c shutdown 2>&1" | grep -oE ": [0-9a-f]{8}" | tr -d ': '; }

P=$(frames_of "$SNESP")
case "$P" in 20*|24*|30*) ;; *) echo "no ROM running (snes=0x$P)"; exit 1;; esac
F=$(printf '0x%x' $((0x$P + 56)))
f0=$((16#$(frames_of $F)))
A0=$(read_all)
until [ $(( $(printf '%d' 0x$(frames_of $F)) - f0 )) -ge $FRAMES ]; do sleep 3; done
A1=$(read_all)
python3 - "$FRAMES" <<PY
import sys
a0="""$A0""".split(); a1="""$A1""".split()
names="frames_ptr ins pulls noise_pulls rev underruns".split()
d=[int(y,16)-int(x,16) for x,y in zip(a0,a1)]
print(f"$ARM  ({sys.argv[1]} frames)")
for n,v in list(zip(names,d))[1:]:
    print(f"  {n:12} {v:>8}")
PY
