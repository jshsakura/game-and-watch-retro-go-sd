#!/usr/bin/env bash
# One command, one answer: which ROM is running, at what frame rate, and where
# the time goes. Run it from the repo root with the ELF that is on the device.
#
#   tools/gnw_probe/measure.sh [seconds] [samples]
#
# It reads ACTIVE_FILE and g_line_cache_frame straight out of the running
# machine, so a measurement labels itself -- no one has to remember which build
# was flashed or which game was up. That mattered: two runs at "59.4 fps" were
# nearly filed as the same game.
set -euo pipefail
cd "$(dirname "$0")/../.."

HOST=${PROBE_HOST:-rpi-genie5}
ELF=${ELF:-build/gw_retro_go.elf}
SECS=${1:-8}
SAMPLES=${2:-700}
OC="sudo openocd -f interface/stlink-dap.cfg -f target/stm32h7x.cfg -c 'adapter speed 4000'"

sym() { arm-none-eabi-objdump -t "$ELF" 2>/dev/null | awk -v n="$1" '$NF==n{print $1; exit}'; }

frame=$(sym g_line_cache_frame)
active=$(sym ACTIVE_FILE)
[ -n "$frame" ] || { echo "no g_line_cache_frame in $ELF"; exit 1; }

# ROM name: ACTIVE_FILE is a pointer to the file record, whose name is at its head.
ptr=$(ssh "$HOST" "$OC -c init -c 'mdw 0x$active' -c shutdown 2>&1" \
      | grep -oE ": [0-9a-f]{8}" | tr -d ': ')
name=$(ssh "$HOST" "$OC -c init -c 'mdb 0x$ptr 64' -c shutdown 2>&1" \
       | grep -oE ' [0-9a-f]{2}' | tr -d ' ' | xxd -r -p 2>/dev/null | tr -c '[:print:]' '.' || true)

read -r a b < <(ssh "$HOST" "$OC -c init -c 'mdw 0x$frame' -c 'sleep $((SECS*1000))' \
                 -c 'mdw 0x$frame' -c shutdown 2>&1" | grep -oE ": [0-9a-f]+" | tr -d ': ' | tr '\n' ' ')
fps=$(python3 -c "print(f'{(0x$b-0x$a)/$SECS:.2f}')")

echo "ROM : $name"
echo "FPS : $fps   (audio-DMA pacing cap is 60.15)"
echo
ssh "$HOST" "timeout 400 ~/gnw_probe.sh sample /tmp/gw.elf $SAMPLES >/dev/null 2>&1"
scp -q "$HOST":/tmp/gnw_pcs.txt /tmp/gnw_pcs.txt
python3 tools/gnw_probe/resolve_snes.py "$ELF" /tmp/gnw_pcs.txt | head -14
