#!/usr/bin/env bash
# Start a ROM on the device from here, with no one at the console.
#
#   start_rom.sh <elf> "/roms/snes/....smc"
#
# The launcher will not take a synthetic button press -- the menu wants real
# elapsed time between polls and the CPU is stopped while the debugger holds it.
# So skip the menu: write the path into spare RAM, call emulator_get_file() for
# the file record, then jump into emulator_start() with the record in r0. It
# never returns; it becomes the game.
#
# Spare RAM is RAM_EMU, which is exactly what is NOT in use while the launcher
# is up -- no overlay is loaded, that is the whole point of the region.
set -euo pipefail
cd "$(dirname "$0")/../.."

HOST=${PROBE_HOST:-rpi-genie5}
ELF=${1:?usage: start_rom.sh <elf> <rom-path>}
ROM=${2:?usage: start_rom.sh <elf> <rom-path>}
SCRATCH=${SCRATCH:-0x24070000}
OC="sudo openocd -f interface/stlink-dap.cfg -f target/stm32h7x.cfg -c 'adapter speed 4000'"

sym() { arm-none-eabi-objdump -t "$ELF" | awk -v n="$1" '$NF==n && $1 ~ /^[0-9a-f]+$/ {print "0x"$1; exit}'; }
GET=$(sym emulator_get_file)
START=$(sym emulator_start)
TRAP=$(sym Reset_Handler)
[ -n "$GET" ] && [ -n "$START" ] && [ -n "$TRAP" ] || { echo "missing symbols: get=$GET start=$START trap=$TRAP"; exit 1; }

# The path, byte by byte, NUL terminated.
writes=""
i=0
while IFS= read -r -n1 -d '' byte; do :; done < /dev/null   # noop, keeps shellcheck quiet
for b in $(printf '%s\0' "$ROM" | od -An -tu1 -v); do
  writes="$writes -c 'mwb $((SCRATCH + i)) $b'"
  i=$((i + 1))
done

echo "[start] $ROM  ($i bytes) -> $SCRATCH"
echo "[start] emulator_get_file=$GET emulator_start=$START trap=$TRAP"

file=$(ssh "$HOST" "$OC -c init -c halt $writes \
  -c 'bp $TRAP 2 hw' -c 'reg r0 $SCRATCH' -c 'reg lr $TRAP' -c 'reg pc $GET' \
  -c resume -c 'wait_halt 5000' -c 'reg r0' -c 'rbp $TRAP' -c shutdown 2>&1" \
  | grep -oE 'r0 \(/32\): 0x[0-9a-f]+' | tail -1 | grep -oE '0x[0-9a-f]+')

echo "[start] emulator_get_file returned $file"
[ -n "$file" ] && [ "$file" != "0x00000000" ] || { echo "[start] no such ROM in the launcher's list"; exit 1; }

ssh "$HOST" "$OC -c init -c halt \
  -c 'reg r0 $file' -c 'reg r1 0' -c 'reg r2 0' -c 'reg r3 0xffffffff' \
  -c 'reg pc $START' -c resume -c shutdown >/dev/null 2>&1" || true
echo "[start] running"
