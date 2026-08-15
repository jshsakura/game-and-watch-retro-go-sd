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
# Spare RAM: RAM_EMU is free of the OVERLAY while the launcher is up, but not
# of the launcher itself -- emulators[] and the 1000-slot shared_files ROM
# list are allocated from its start. A scratch inside the scanned region gets
# its bytes overwritten by emulator_get_file()'s list refresh before the
# strcmp loop reaches the path, and the call returns NULL for a ROM that
# f_stat says is right there. The final 4 KB of the region sits past every
# allocation the launcher makes (list, cores) until a list grows past ~815
# entries; the first call of a session also needs the window after
# emulators_init() and before any core start (emulators goes NULL once a core
# runs). When calling emulator_start() directly, set pc with bit0 (Thumb):
# an even pc resumed with EPSR.T clear enters as INVALID STATE UsageFault.
set -euo pipefail
cd "$(dirname "$0")/../.."

HOST=${PROBE_HOST:-rpi-genie5}
ELF=${1:?usage: start_rom.sh <elf> <rom-path>}
ROM=${2:?usage: start_rom.sh <elf> <rom-path>}
SCRATCH=${SCRATCH:-0x240ff000}
OC="sudo openocd -f interface/stlink-dap.cfg -f target/stm32h7x.cfg -c 'adapter speed 4000'"

sym() { arm-none-eabi-objdump -t "$ELF" | awk -v n="$1" '$NF==n && $1 ~ /^[0-9a-f]+$/ && !f {print "0x"$1; f=1}'; }
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
