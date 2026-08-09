#!/usr/bin/env bash
# Scene-matched device benchmark: how long does this build take to emulate the
# same N frames?
#
#   bench.sh <elf> [frames]
#
# Wall-clock fps is not comparable between builds unless both are looking at the
# same thing, and they never are: the console boots into a title screen and an
# attract demo, so at any given second two arms are in different scenes. Two
# readings of "51.4" and "50.1" told us nothing today for exactly that reason.
#
# With no controller input the emulation is deterministic, so frame 1800 is the
# same frame in every build. Reset, wait for the counter to reach N, and time it.
# The workload is then identical by construction and the only variable is the code.
set -eu   # NOT pipefail: awk|head closes objdump's pipe and SIGPIPE is not a failure
cd "$(dirname "$0")/../.."

HOST=${PROBE_HOST:-rpi-genie5}
ELF=${1:?usage: bench.sh <elf> [frames]}
FRAMES=${2:-1800}
OC="sudo openocd -f interface/stlink-dap.cfg -f target/stm32h7x.cfg -c 'adapter speed 4000'"

# Prefer the core's own frame counter: it exists in every configuration, while
# g_line_cache_frame vanishes with SNES_LINE_CACHE=0 and took the benchmark with
# it. `snes` is a pointer, so read it first and add offsetof(Snes, frames).
SNESP=$(arm-none-eabi-objdump -t "$ELF" | awk '$NF=="snes" && $1 ~ /^[0-9a-f]+$/ {print "0x"$1}' | head -1)
FRAMES_OFF=${FRAMES_OFF:-56}
if [ -n "$SNESP" ]; then
  P=$(ssh "$HOST" "$OC -c init -c 'mdw $SNESP' -c shutdown 2>&1" | grep -oE ": [0-9a-f]{8}" | tr -d ': ')
  # Non-zero is not the same as valid. With no ROM running (launcher up, or a
  # different image flashed than the ELF names) this word is whatever the last
  # overlay left behind -- 0xfbdef027 once, which the old test accepted and then
  # polled forever. snes_init mallocs the struct, so the pointer is DTCM
  # (0x20xxxxxx), AHB (0x30xxxxxx) or the overlay's own BSS (0x24xxxxxx).
  case "$P" in
    20*|24*|30*) F=$(printf '0x%x' $((0x$P + FRAMES_OFF))) ;;
    *) echo "snes = 0x${P:-?} is not a heap pointer: no SNES ROM is running" >&2 ;;
  esac
fi
if [ -z "${F:-}" ]; then
  F=$(arm-none-eabi-objdump -t "$ELF" | awk '$NF=="g_line_cache_frame"{print "0x"$1}' | head -1)
fi
[ -n "$F" ] || { echo "no frame counter found in $ELF"; exit 1; }

# Measure a DELTA, not an absolute count. The old test waited for the counter to
# fall below 100 as its "the reset has happened" signal, which works only for a
# cold boot: with GNW_AUTOBOOT_STATE the savestate restores snes->frames too, so
# the counter comes back at whatever the state was saved at, the wait falls
# straight through, and the run reports 700 fps -- the shape of a benchmark
# measuring nothing, again. Read the counter once the ROM is alive, then time
# FRAMES more from there.
#
# Both waits are BOUNDED, and that is not a detail. Unbounded, this script reset
# the device and then polled a counter that was never going to move, for as long
# as it was left running -- while the firmware's own anti-brick counter watched
# reset after reset fail to reach a successful boot. Three of those and the
# device stops at the rescue screen before the ROM ever loads, and powers itself
# off a minute later; the benchmark's answer to "why is there no frame counter"
# was to reset and try again, which is precisely the wrong move. A benchmark
# that cannot tell "this build is slow" from "this build does not boot" will
# eventually brick the thing it is measuring. Give up and say which it was.
BOOT_TIMEOUT_MS=${BOOT_TIMEOUT_MS:-20000}
RUN_TIMEOUT_MS=${RUN_TIMEOUT_MS:-$((FRAMES * 40 + 20000))}

# Settle first: reset, let the ROM (or the savestate) come up, and only then
# take the base reading and start the clock. The reset and OpenOCD's start-up
# are thus OUTSIDE the timed window, unlike before -- which also makes short
# runs comparable with long ones.
read -r base t_base <<EOF
$(ssh "$HOST" "$OC -c init -c 'reset run' \
  -c 'set t 0; set a [lindex [read_memory $F 32 1] 0]
      while {\$t < $BOOT_TIMEOUT_MS} { sleep 200; incr t 200
        set b [lindex [read_memory $F 32 1] 0]
        if {[expr {\$b != \$a && \$b > \$a && \$b - \$a < 1000}]} { break }
        set a \$b }' \
  -c 'echo BASE=[lindex [read_memory $F 32 1] 0]' \
  -c shutdown" 2>&1 | sed -n 's/^BASE=\(0x[0-9a-fA-F]*\|[0-9]*\).*/\1 x/p' | head -1)
EOF
if [ -z "${base:-}" ]; then
  echo "no frame counter movement after ${BOOT_TIMEOUT_MS} ms: this build does not" >&2
  echo "reach the ROM. Do NOT just run it again -- each reset counts as a failed" >&2
  echo "boot, and the third parks the device at the rescue screen. Flash a" >&2
  echo "known-good arm instead." >&2
  exit 2
fi
target=$((base + FRAMES))

start=$(date +%s.%N)
out=$(ssh "$HOST" "$OC -c init \
  -c 'set t 0; while {[lindex [read_memory $F 32 1] 0] < $target && \$t < $RUN_TIMEOUT_MS} { sleep 100; incr t 100 }' \
  -c 'if {[lindex [read_memory $F 32 1] 0] < $target} { echo BENCH_TOO_SLOW }' \
  -c shutdown" 2>&1) || true
end=$(date +%s.%N)

case "$out" in
  *BENCH_TOO_SLOW*)
    echo "reached only part of $FRAMES frames in ${RUN_TIMEOUT_MS} ms -- build is" >&2
    echo "running but far slower than any real arm. Not reporting a rate for it." >&2
    exit 3 ;;
esac

python3 -c "
t = $end - $start
# The reset, OpenOCD's own start-up and the poll granularity are in there too;
# they are the same for both arms, which is what makes the comparison fair.
print(f'{$FRAMES} frames in {t:.2f} s  ->  {$FRAMES/t:.2f} fps')"
