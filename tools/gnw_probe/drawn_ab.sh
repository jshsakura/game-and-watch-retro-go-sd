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

# Is the arm we are about to resolve symbols against the one actually FLASHED?
#
# Nothing checked this until now, and with two sessions sharing one console it
# went wrong for real: a whole VB debugging round was spent reading THIS repo's
# SNES binary through another arm's ELF, and the PC that looked like a VB BSS
# address was inside .overlay_snes. Every symbol lookup below is a lie when the
# device holds a different image, and the failure has no other symptom.
#
# 4 KB off the front of internal flash is enough to separate two builds -- the
# vector table and the first of the resident code -- and costs about a second.
# Skips (loudly) if the dump cannot be taken, per the rule that a safety net
# must not be the thing that breaks the run.
verify_flashed() {
  local bin="/tmp/gnw_arms/$ARM/gw_retro_go_intflash.bin"
  [ -f "$bin" ] || { echo "$ARM: no intflash.bin to verify against -- SKIPPED" >&2; return 0; }
  local n=4096
  if ! ssh -n "$HOST" "python3 -m gnwmanager dump 0x08100000 --dst /tmp/gnw_flashed.bin --size $n" \
       >/dev/null 2>&1; then
    echo "$ARM: could not read flash to verify the image -- SKIPPED" >&2
    return 0
  fi
  scp -q "$HOST:/tmp/gnw_flashed.bin" /tmp/gnw_flashed.bin 2>/dev/null || {
    echo "$ARM: could not fetch the flash dump -- SKIPPED" >&2; return 0; }
  head -c $n "$bin" > /tmp/gnw_armhead.bin
  if ! cmp -s /tmp/gnw_flashed.bin /tmp/gnw_armhead.bin; then
    echo "$ARM: THE DEVICE IS NOT RUNNING THIS ARM. The first $n bytes of" >&2
    echo "      internal flash differ from $bin." >&2
    echo "      Every symbol below would be resolved against the wrong program." >&2
    return 5
  fi
}
verify_flashed || exit $?

sym() { arm-none-eabi-nm "$E" | awk -v s="$1" '$3==s{print "0x"$1; exit}'; }

# THE COUNTERS: always the shared pair in Core/Src/porting/common.c.
#
# This used to choose between that pair and the SNES core's own
# g_snes_drawn_frames by reading the CONTENTS of the `snes` pointer and calling
# it SNES when they looked like a RAM address (20/24/30…). `snes` lives at
# 0x240c5bc4 — inside RAM_EMU, which every core's overlay shares — so with any
# other core running those bytes are that core's data, and about 3 values in 256
# impersonate a RAM pointer. The usual outcome is a loud death ("frame counter
# not moving"); the unlucky one is a number read out of a struct that happens to
# keep changing, which is how 32X Doom once measured emu=360 drawn=0.
#
# There is nothing left to choose between. SNES's only lcd_swap()
# (main_snes.c:1404) sits inside `if (drawFrame)` — the same condition that
# increments g_snes_drawn_frames at :1361 — so the two count the same event. The
# shared pair is in DTCM, resident, where no overlay can alias it.
DRAWN=$(sym g_common_drawn_frames)
F=$(sym g_common_emu_frames)
[ -n "$DRAWN" ] && [ -n "$F" ] || { echo "$ARM: no shared counters in $E" >&2; exit 1; }

# DID THE GUARD ASK FOR SKIPS? Without this the headline is ambiguous in the one
# direction that matters: a core running at full speed draws every frame it
# emulates, so ratio 1.0000 means EITHER "the drawn counter is over-reporting" OR
# "there was nothing to skip", and those demand opposite conclusions.
#
# Read what this is, exactly. common_emu_state.skipped_frames accumulates
# `skip_frames` at the END of common_emu_frame_loop() (common.c:215), where it
# has just been recomputed from the integrator — so it counts what the guard
# DEMANDED, not what any core did about it. skipped > 0 therefore does NOT mean
# a frame was skipped. Genesis is the proof: main_gwenesis.c:777 discards the
# return value entirely (`// bool drawFrame =`) and :913 zeroes skip_frames every
# iteration, so MD renders every frame while this counter keeps climbing.
#
# So it answers one question only, and it is the question that makes a window
# worth measuring: skipped == 0 means the guard never asked, and nothing about
# the drawn counter can be concluded from that window. To conclude a DEFECT you
# need this AND a core that reads drawFrame — which is a source property, not a
# runtime one, and is what tests/lcd_swap_audited.txt records.
#
# One word covers it: skipped_frames is a uint16 at offset 8 and frame_time_10us
# an int16 at offset 10, so a single mdw reads the demand in the low half and the
# frame budget in the high half — and the budget is what a speedup setting halves
# to make the guard ask at all.
STATE=$(sym common_emu_state)
GUARD=${STATE:+$(printf '0x%x' $((STATE + 8)))}

rd() { ssh "$HOST" "$OC -c init $(for a in "$@"; do printf -- "-c 'mdw %s' " "$a"; done) -c shutdown 2>&1" \
       | grep -oE ": [0-9a-f]{8}" | tr -d ': '; }

# THE SCENE LABEL: kept, but best-effort and never fatal. Whether the window is
# the savestate scene or a cold boot has cost this tree three times (a cold-boot
# window understates SNES spin by 5.4x), so it does not get dropped. But it IS a
# label, and it is read from g_snes_state_resumed — an overlay address, only
# meaningful while SNES is the loaded core — so it has to be gated on which core
# is running.
#
# currentApp (Core/Src/porting/odroid_system.c) is resident DTCM and `id` is its
# first member, so the struct's address is the id's. The APPID number is parsed
# out of appid.h at run time rather than written here: APPIDs shift when one is
# added, and a number hardcoded in this file would go quietly wrong — the same
# disease as the counter above. An empty parse means "unknown", never an exit.
APPP=$(sym currentApp)
RESUMED=$(sym g_snes_state_resumed); [ -n "$RESUMED" ] || RESUMED=$(sym gsnes__g_snes_state_resumed)
SNES_ID=
# shellcheck source=/dev/null
if [ -r "$(dirname "$0")/appid.sh" ]; then
  . "$(dirname "$0")/appid.sh"
  SNES_ID=$(appid_value SNES)
fi

# Reset first. Aligning the frame COUNT is not enough: without a reset the
# window still starts wherever the attract demo happens to be, and the same
# build read ratio 0.3623 and 0.4371 on two consecutive aligned windows. From a
# reset both arms start at the same frame of the same scene.
ssh "$HOST" "$OC -c init -c 'reset run' -c shutdown" >/dev/null 2>&1
# Let the ROM get past its first frames, then start: the counter must be moving.
t=0
until [ $t -ge 30 ]; do
  sleep 2; t=$((t+2))
  a=$(rd "$F"); sleep 2; b=$(rd "$F")
  [ "$a" != "$b" ] && break
done
[ "$a" != "$b" ] || { echo "$ARM: frame counter not moving after reset" >&2; exit 2; }

# AFTER the reset, not before. Read first, this reported the PREVIOUS run's
# scene -- in a sweep, the previous cartridge's -- and produced an A/B where one
# arm said savestate and the other COLD for the same ROM.
SCENE=unknown
if [ -n "$APPP" ] && [ -n "$SNES_ID" ] && [ -n "$RESUMED" ] \
   && [ "$((0x$(rd "$APPP")))" = "$SNES_ID" ]; then
  [ "$(rd "$RESUMED")" = "00000001" ] && SCENE=savestate || SCENE="COLD BOOT (not the play scene)"
fi

read -r f0 d0 g0 <<EOF
$(rd "$F" "$DRAWN" ${GUARD:+"$GUARD"} | tr '\n' ' ')
EOF
start=$(date +%s.%N)
target=$(( 0x$f0 + FRAMES ))
t=0
until [ $(( 0x$(rd "$F") )) -ge $target ] || [ $t -ge 120 ]; do sleep 3; t=$((t+3)); done
end=$(date +%s.%N)
read -r f1 d1 g1 <<EOF
$(rd "$F" "$DRAWN" ${GUARD:+"$GUARD"} | tr '\n' ' ')
EOF

python3 -c "
emu = 0x$f1 - 0x$f0
drawn = 0x$d1 - 0x$d0
t = $end - $start
line = (f'$ARM  scene=$SCENE  emu={emu} drawn={drawn}  ratio={drawn/emu:.4f}'
        f'  ({emu/t:.2f} emulated fps, {drawn/t:.2f} drawn fps over {t:.1f}s)')
g0, g1 = '${g0:-}', '${g1:-}'
if g0 and g1:
    # low half = skipped_frames (uint16, wraps), high half = frame_time_10us
    skipped = (int(g1, 16) & 0xffff) - (int(g0, 16) & 0xffff) & 0xffff
    budget = int(g1, 16) >> 16
    line += f'  [guard demanded {skipped} skips, frame_budget={budget/100:.2f}ms]'
    if skipped == 0:
        # Not a footnote: with nothing demanded, ratio 1.0 says only that the
        # core kept up. It cannot distinguish an honest counter from one that
        # reports every flip as a drawn frame, which is the whole question this
        # tool is pointed at. Halve the frame budget (a speedup setting) and
        # measure again.
        line += '  <-- GUARD NEVER ASKED: this window cannot judge the counter'
print(line)"
