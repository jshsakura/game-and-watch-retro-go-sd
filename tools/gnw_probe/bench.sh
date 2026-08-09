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

F=$(arm-none-eabi-objdump -t "$ELF" | awk '$NF=="g_line_cache_frame"{print "0x"$1}' | head -1)
[ -n "$F" ] || { echo "no g_line_cache_frame in $ELF"; exit 1; }

# Wait for the counter to go back to zero first. Without this the timer starts
# with the PREVIOUS run's count already past the target and returns instantly --
# it read 3151 fps once, which is the shape of a benchmark measuring nothing.
start=$(date +%s.%N)
ssh "$HOST" "$OC -c init -c 'reset run' \
  -c 'while {[lindex [read_memory $F 32 1] 0] > 100} { sleep 100 }' \
  -c 'while {[lindex [read_memory $F 32 1] 0] < $FRAMES} { sleep 100 }' \
  -c shutdown" >/dev/null 2>&1
end=$(date +%s.%N)

python3 -c "
t = $end - $start
# The reset, OpenOCD's own start-up and the poll granularity are in there too;
# they are the same for both arms, which is what makes the comparison fair.
print(f'{$FRAMES} frames in {t:.2f} s  ->  {$FRAMES/t:.2f} fps')"
