#!/usr/bin/env bash
# Frame-aligned drawn-frame A/B.
#
#   drawn_ab.sh <arm> [frames] [--no-reset]
#
# drawrate.sh samples both counters over a WALL-CLOCK window, which is the same
# mistake stretch_ab.sh was written to fix for the audio counters. Measured
# today on Super Mario World, the identical build read 20.73, 25.27, 21.70,
# 28.04, 24.40, 26.22 and 25.12 drawn fps across seven such windows -- a spread
# of 7.3 fps, wider than any difference worth judging. The console free-runs
# through a title screen and a looping attract demo, so a 20-second window lands
# somewhere different every time.
#
# The scene is deterministic in EMULATED frames, so take the delta across a
# fixed number of them instead: reset, let the ROM come up, read (drawn,
# frames), wait for exactly N more emulated frames, read again. Both arms then
# see the same N frames of the same scene, and drawn/emulated is comparable.
#
# The absolute drawn FPS still needs wall time, which is reported too -- but the
# ratio is the number to compare, because emulated fps is pinned at the audio
# cap on any ROM fast enough to matter here.
set -eu
ARM=${1:?usage: drawn_ab.sh <arm> [frames]}
FRAMES=${2:-1800}
HOST=${PROBE_HOST:-rpi-genie5}
E=/tmp/gnw_arms/$ARM/gw_retro_go.elf
OC="sudo openocd -f interface/stlink-dap.cfg -f target/stm32h7x.cfg -c 'adapter speed 4000'"

sym() { arm-none-eabi-nm "$E" | awk -v s="$1" '$3==s{print "0x"$1; exit}'; }
SNESP=$(sym snes)
DRAWN=$(sym g_snes_drawn_frames); [ -n "$DRAWN" ] || DRAWN=$(sym gsnes__g_snes_drawn_frames)
# Any other core: the shared counters in Core/Src/porting/common.c. The SNES
# pair is preferred only because it predates them and every earlier number in
# the log was taken from it.
COMMON_DRAWN=$(sym g_common_drawn_frames)
COMMON_EMU=$(sym g_common_emu_frames)
[ -n "$DRAWN" ] || DRAWN=$COMMON_DRAWN
RESUMED=$(sym g_snes_state_resumed); [ -n "$RESUMED" ] || RESUMED=$(sym gsnes__g_snes_state_resumed)
[ -n "$SNESP" ] && [ -n "$DRAWN" ] || { echo "$ARM: no counters in $E" >&2; exit 1; }

rd() { ssh "$HOST" "$OC -c init $(for a in "$@"; do printf -- "-c 'mdw %s' " "$a"; done) -c shutdown 2>&1" \
       | grep -oE ": [0-9a-f]{8}" | tr -d ': '; }

P=${SNESP:+$(rd "$SNESP")}
case "${P:-}" in
  20*|24*|30*) F=$(printf '0x%x' $((0x$P + 56))) ;;
  *) [ -n "$COMMON_EMU" ] || { echo "$ARM: no frame counter (snes=0x${P:-none})" >&2; exit 1; }
     F=$COMMON_EMU ;;   # any non-SNES core
esac

# Say out loud whether this is the savestate scene or a cold boot. Super Mario
# World's slot-0 state is refused by this build, and a cold-boot window is a
# different machine -- a fact this tree has now paid for three times.

# Reset first. Aligning the frame COUNT is not enough: without a reset the
# window still starts wherever the attract demo happens to be, and the same
# build read ratio 0.3623 and 0.4371 on two consecutive aligned windows. From a
# reset both arms start at the same frame of the same scene.
ssh "$HOST" "$OC -c init -c 'reset run' -c shutdown" >/dev/null 2>&1
t=0
until [ $t -ge 30 ]; do
  sleep 2; t=$((t+2))
  if [ -n "$SNESP" ]; then
    P=$(rd "$SNESP"); case "$P" in 20*|24*|30*) F=$(printf '0x%x' $((0x$P + 56))); break ;; esac
  else
    F=$COMMON_EMU; break
  fi
done
# Let the ROM get past its first frames, then start: the counter must be moving.
a=$(rd "$F"); sleep 2; b=$(rd "$F")
[ "$a" = "$b" ] && { echo "$ARM: frame counter not moving after reset" >&2; exit 2; }

# AFTER the reset, not before. Read first, this reported the PREVIOUS run's
# scene -- in a sweep, the previous cartridge's -- and produced an A/B where one
# arm said savestate and the other COLD for the same ROM.
if [ -n "$RESUMED" ]; then
  [ "$(rd "$RESUMED")" = "00000001" ] && SCENE=savestate || SCENE="COLD BOOT (not the play scene)"
else
  SCENE=unknown
fi

read -r f0 d0 <<EOF
$(rd "$F" "$DRAWN" | tr '\n' ' ')
EOF
start=$(date +%s.%N)
target=$(( 0x$f0 + FRAMES ))
t=0
until [ $(( 0x$(rd "$F") )) -ge $target ] || [ $t -ge 120 ]; do sleep 3; t=$((t+3)); done
end=$(date +%s.%N)
read -r f1 d1 <<EOF
$(rd "$F" "$DRAWN" | tr '\n' ' ')
EOF

python3 -c "
emu = 0x$f1 - 0x$f0
drawn = 0x$d1 - 0x$d0
t = $end - $start
print(f'$ARM  scene=$SCENE  emu={emu} drawn={drawn}  ratio={drawn/emu:.4f}'
      f'  ({emu/t:.2f} emulated fps, {drawn/t:.2f} drawn fps over {t:.1f}s)')"
