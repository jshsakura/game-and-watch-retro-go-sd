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
# 4 KB off the front of internal flash separates two builds that differ in
# RESIDENT code, and costs about a second. Skips (loudly) if the dump cannot be
# taken, per the rule that a safety net must not be the thing that breaks the
# run.
#
# KNOW WHAT THIS DOES NOT SEE. Read the map for those 4096 bytes:
#
#   .isr_vector    0x08100000  0x2ac    startup_stm32h7b0xx.o
#   .firmware_abi  0x08100400  0x20c    gw_firmware_abi.o
#   .text          0x08100610  …        memcpy-armv7m.o, libc_nano, libgcc
#
# Startup asm, the ABI table, and hand-written memcpy. **A core knob does not
# live there.** An ablation like SNES_PPU_VIRGIN_Z or a VB switch lands in
# `.overlay_<core>`, whose LMA is external flash and whose runtime copy comes off
# the SD card -- so the two arms of exactly the A/B this tool exists to run can
# have byte-identical intflash heads. Passing here is not evidence the device is
# running this arm's CORE.
#
# That is not hypothetical: the failure this whole file keeps paying for is a
# stale `cores/<name>.bin` on the card under a different arm's flash (see
# arm.sh's PUSH_CORE note, and the day Virtual Boy was measured twice through
# another arm's core). So name the core you are measuring --
#
#   CORE=vb drawn_ab.sh <arm>
#
# -- and the card's copy is hashed against this arm's. Without it the card side
# is unverified and this says so rather than implying a clean bill.
INTFLASH_ADDR=${INTFLASH_ADDR:-0x08100000}   # INTFLASH_BANK=1 arms: pass 0x08000000
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# BLIND SPOT, on purpose: this checks the RESIDENT image only. A knob that
# touches just one core's overlay (MD32X_FORCED_DRAW_RATIO, SNES_LINE_HIRQ, any
# core-local define) leaves the two arms' intflash byte-identical and differs
# only in cores/<name>.bin -- and then this returns GREEN for either one. Use
# `PUSH_CORE=<name> arm.sh flash <arm>` so the card matches by construction; the
# card's core file has been the wrong program three times in one day.
verify_flashed() {
  local bin="/tmp/gnw_arms/$ARM/gw_retro_go_intflash.bin"
  [ -f "$bin" ] || { echo "$ARM: no intflash.bin to verify against -- SKIPPED" >&2; return 0; }
  local n=4096
  # Remote and local scratch are per-run: two sessions share one console here,
  # which is the very scenario above, and fixed /tmp names made them race and
  # compare one session's dump against the other's arm head.
  local rdump="/tmp/gnw_flashed.$$.bin"
  if ! ssh -n "$HOST" "python3 -m gnwmanager dump $INTFLASH_ADDR --dst $rdump --size $n" \
       >/dev/null 2>&1; then
    echo "$ARM: could not read flash to verify the image -- SKIPPED" >&2
    return 0
  fi
  scp -q "$HOST:$rdump" "$TMP/flashed.bin" 2>/dev/null || {
    echo "$ARM: could not fetch the flash dump -- SKIPPED" >&2; return 0; }
  ssh -n "$HOST" "rm -f $rdump" >/dev/null 2>&1 || true
  head -c $n "$bin" > "$TMP/armhead.bin"
  if ! cmp -s "$TMP/flashed.bin" "$TMP/armhead.bin"; then
    echo "$ARM: THE DEVICE IS NOT RUNNING THIS ARM. The first $n bytes of" >&2
    echo "      internal flash differ from $bin." >&2
    echo "      Every symbol below would be resolved against the wrong program." >&2
    return 5
  fi
}
verify_core_on_card() {
  if [ -z "${CORE:-}" ]; then
    echo "$ARM: card side UNVERIFIED (pass CORE=<name> to hash /cores/<name>.bin)" >&2
    return 0
  fi
  local mine="/tmp/gnw_arms/$ARM/cores/$CORE.bin"
  [ -f "$mine" ] || { echo "$ARM: no cores/$CORE.bin in this arm -- SKIPPED" >&2; return 0; }
  local rpull="/tmp/gnw_card_$CORE.$$.bin"
  if ! ssh -n "$HOST" "python3 -m gnwmanager sdpull /cores/$CORE.bin $rpull" >/dev/null 2>&1; then
    echo "$ARM: could not read /cores/$CORE.bin off the card -- SKIPPED" >&2
    return 0
  fi
  scp -q "$HOST:$rpull" "$TMP/card_core.bin" 2>/dev/null || {
    echo "$ARM: could not fetch the card's core -- SKIPPED" >&2; return 0; }
  ssh -n "$HOST" "rm -f $rpull" >/dev/null 2>&1 || true
  if ! cmp -s "$TMP/card_core.bin" "$mine"; then
    echo "$ARM: THE CARD'S cores/$CORE.bin IS NOT THIS ARM'S." >&2
    echo "      The flash may match while the core does not -- that pairing is" >&2
    echo "      what the measurement actually runs. Re-flash with PUSH_CORE=$CORE." >&2
    return 6
  fi
}
verify_flashed || exit $?
verify_core_on_card || exit $?

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
# Read what this is, exactly — it is weaker than it looks, in TWO ways.
#
# First, common_emu_state.skipped_frames accumulates `skip_frames` at the END of
# common_emu_frame_loop() (common.c:215), where it has just been recomputed from
# the integrator — so it counts what the guard DEMANDED, not what any core did
# about it. Genesis is the proof: main_gwenesis.c:777 discards the return value
# entirely (`// bool drawFrame =`) and :913 zeroes skip_frames every iteration,
# so MD renders every frame while this counter keeps climbing.
#
# Second — and this killed a verdict rule that shipped in this file — the guard
# withholds a frame only at skip_frames == 2 (`draw_frame = skip_frames < 2`,
# common.c:142), but common.c:213 also assigns **1**, one budget of lateness,
# and :215 accumulates 1 and 2 alike. A frame charged 1 is still DRAWN. So a
# window where the integrator crossed one budget but never two reads
# `demanded > 0` with every frame drawn — on a correct build. Over 1800 frames
# that is close to certain. "ratio == 1.0000 AND demanded > 0 proves the counter
# over-reports" was written here and is FALSE for exactly that reason.
#
# What survives is only the negative: **demanded == 0 means the guard never came
# under pressure at all**, so that window cannot judge the drawn counter and
# there is nothing to read. The positive direction needs the ratio itself: on a
# core that obeys the guard, `drawn < emu` IS the guard withholding, and that is
# the signal. Hence the order for an A/B — raise the speedup on the GOOD arm
# until ratio < 1 (proving frames are actually being withheld at that setting),
# then run the suspect arm at the same setting and see whether it reports
# 1.0000. And a defect still needs a core that reads drawFrame, which is a
# source property recorded in tests/lcd_swap_audited.txt, not a runtime one.
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
  # appid.sh defaults APPID_H to a RELATIVE path, so sourcing it from anywhere
  # but the repo root silently yields no value at all -- and the failure mode of
  # "no value" is SCENE=unknown for ever, i.e. the cold-boot window quietly
  # readable as a play scene again. Point it at the header next to this script.
  APPID_H=${APPID_H:-$(cd "$(dirname "$0")/../.." && pwd)/Core/Inc/retro-go/appid.h}
  export APPID_H
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

# IS THE DEVICE RUNNING THE CORE YOU NAMED?
#
# Everything above checks that the right PROGRAM is on the device: the resident
# image, and (with CORE=) the core file on the card. None of it checks which
# core the device actually LAUNCHED, and the counters are shared, so a run
# aimed at one core will happily report another one's frames.
#
# Measured, not theorised: a bench file reading `msx:1942.rom` does not match --
# the launcher compares the name without its extension -- so the lookup failed,
# autoboot fell through to SNES ROM index 0, and this tool printed 0.6684 and
# 0.6808 for two arms of an MSX A/B. The numbers were plausible, repeatable, and
# about a core that never reaches the code under test. The arms agreed because
# the SNES core does not call lcd_swap_stale() at all.
#
# currentApp.id is resident and `id` is its first member. The APPID comes from
# the header at run time (never hardcoded -- APPIDs shift when one is added).
# If the name cannot be resolved this says UNVERIFIED and continues; if it CAN
# be resolved and disagrees, that is not a caveat, it is the wrong measurement.
if [ -n "${CORE:-}" ] && [ -n "$APPP" ]; then
  want_id=$(appid_value "$(printf '%s' "$CORE" | tr '[:lower:]' '[:upper:]')" 2>/dev/null || true)
  if [ -z "${want_id:-}" ]; then
    echo "$ARM: running core UNVERIFIED (no APPID for '$CORE' in the header)" >&2
  else
    got_id=$((0x$(rd "$APPP")))
    if [ "$got_id" != "$want_id" ]; then
      echo "$ARM: THE DEVICE IS NOT RUNNING '$CORE'. currentApp.id=$got_id," >&2
      echo "      expected $want_id. The counters are shared, so this would have" >&2
      echo "      reported another core's frames as '$CORE'." >&2
      exit 7
    fi
  fi
fi

# AFTER the reset, not before. Read first, this reported the PREVIOUS run's
# scene -- in a sweep, the previous cartridge's -- and produced an A/B where one
# arm said savestate and the other COLD for the same ROM.
SCENE=unknown
if [ -n "$APPP" ] && [ -n "$SNES_ID" ] && [ -n "$RESUMED" ] \
   && [ "$((0x$(rd "$APPP")))" = "$SNES_ID" ]; then
  [ "$(rd "$RESUMED")" = "00000001" ] && SCENE=savestate || SCENE="COLD BOOT (not the play scene)"
fi

# An empty SWD read is not an error to bash: `0x` evaluates to 0, so a missed
# read would set target=$FRAMES, exit the wait loop on its first poll, and hand
# python `emu = 0x1234 - 0x` -- a SyntaxError naming nothing. Check the reads.
hex8() { case "$1" in [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) return 0 ;; *) return 1 ;; esac; }
# Count as well as shape. rd() emits one word per address, so a PARTIAL failure
# -- one mdw of the three dropping out -- shifts every value one slot left and
# the drawn counter is read as the frame counter. Each surviving word is still
# eight hex digits, so hex8 alone would wave it through.
NVALS=2; [ -n "${GUARD:-}" ] && NVALS=3
readvals() {                      # $1: label for the diagnostic
  local v n
  v=$(rd "$F" "$DRAWN" ${GUARD:+"$GUARD"} | tr '\n' ' ')
  n=$(printf '%s' "$v" | wc -w)
  [ "$n" -eq "$NVALS" ] || { echo "$ARM: SWD read $1 returned $n of $NVALS values ('$v')" >&2; exit 3; }
  printf '%s' "$v"
}

read -r f0 d0 g0 <<EOF
$(readvals first)
EOF
hex8 "${f0:-}" && hex8 "${d0:-}" || { echo "$ARM: SWD read failed (f0='${f0:-}' d0='${d0:-}')" >&2; exit 3; }
# Both timestamps AFTER their read. Taking `end` before the closing read charged
# the window a full ssh+openocd round trip it did not contain, inflating both
# reported fps figures by several percent while leaving the ratio untouched.
start=$(date +%s.%N)
target=$(( 0x$f0 + FRAMES ))
t=0
until [ $(( 0x$(rd "$F") )) -ge $target ] || [ $t -ge 120 ]; do sleep 3; t=$((t+3)); done
truncated=$([ $t -ge 120 ] && echo 1 || echo 0)
read -r f1 d1 g1 <<EOF
$(readvals second)
EOF
end=$(date +%s.%N)
hex8 "${f1:-}" && hex8 "${d1:-}" || { echo "$ARM: SWD read failed (f1='${f1:-}' d1='${d1:-}')" >&2; exit 3; }

python3 -c "
emu = 0x$f1 - 0x$f0
drawn = 0x$d1 - 0x$d0
t = $end - $start
line = (f'$ARM  scene=$SCENE  emu={emu} drawn={drawn}  ratio={drawn/emu:.4f}'
        f'  ({emu/t:.2f} emulated fps, {drawn/t:.2f} drawn fps over {t:.1f}s)')
if $truncated:
    # The whole point of the frame-aligned window (see the header) is that both
    # arms see the SAME N frames of the same scene. The 120 s cap breaks that
    # silently on any core slow enough to need longer -- 1800 frames needs more
    # than 120 s below 15 fps, which the ledger's 32X Doom rows sit under. Then
    # the two arms are back to comparing different-length wall-clock windows,
    # the exact defect this tool was written to remove.
    line += f'  <-- WINDOW TRUNCATED at 120s: {emu} of $FRAMES frames, arms not aligned'
g0, g1 = '${g0:-}', '${g1:-}'
if g0 and g1:
    # low half = skipped_frames (uint16, wraps), high half = frame_time_10us
    skipped = (int(g1, 16) & 0xffff) - (int(g0, 16) & 0xffff) & 0xffff
    budget = int(g1, 16) >> 16
    # "charged" not "skipped": common.c:213 charges 1 for one budget of
    # lateness and that frame is still drawn; only 2 withholds one. The sum
    # cannot tell them apart, so this number is pressure, not skips.
    line += f'  [guard charged {skipped}, frame_budget={budget/100:.2f}ms]'
    if skipped == 0:
        # Not a footnote: with no pressure at all, ratio 1.0 says only that the
        # core kept up. It cannot distinguish an honest counter from one that
        # reports every flip as a drawn frame, which is the whole question this
        # tool is pointed at. Halve the frame budget (a speedup setting) and
        # measure again.
        line += '  <-- GUARD NEVER UNDER PRESSURE: this window cannot judge the counter'
    elif drawn == emu:
        # Pressure but nothing withheld. Do NOT read this as the counter lying:
        # every charge may have been a 1, which draws. It means the setting was
        # not hard enough to make the guard withhold, so the window still cannot
        # judge. Raise the speedup until this arm itself reports ratio < 1.
        line += '  <-- PRESSURE BUT NOTHING WITHHELD: not a verdict, raise the speedup'
if drawn > emu:
    # lcd_swap() is also the launcher's, the clock's, the video and music
    # players' and the pause overlay's. The stated defence is that this is only
    # ever read as a delta with a game running -- which was asserted in a
    # comment and enforced nowhere, so here it is enforced. A core presents at
    # most one frame per frame it emulates, so drawn > emu cannot come from the
    # core: the window caught UI flips, and while a menu is up the emulator loop
    # is not running at all, so the emulated side stalls while the drawn side
    # keeps climbing. The ratio is then meaningless in the flattering direction.
    #
    # And know what it does NOT catch: it only sees flips that push drawn PAST
    # emu. On an arm running below 1.0 the same stray flips are absorbed --
    # a 0.900 window with a battery banner in it reads 0.905, silently. So this
    # is a tripwire for the gross case (a menu left open, an alarm ringing),
    # not a guarantee that a sub-1.0 ratio is free of UI flips. The defence for
    # those is still procedural: do not touch the console during a window.
    #
    # Expect this to fire most often on the arms where it looks least plausible.
    # A core that draws every frame sits at ratio exactly 1.0000 with NO
    # headroom -- 32X's per-core arm, and any core that ignores the guard (VB,
    # WonderSwan, Videopac) -- so one stray flip trips it immediately, while a
    # core running at 0.25 can absorb hundreds. Frequency here is a property of
    # the arm's headroom, not evidence that the check is oversensitive.
    line += '  <-- UI FLIPS IN THE WINDOW (drawn > emu): not a core measurement'
print(line)"
