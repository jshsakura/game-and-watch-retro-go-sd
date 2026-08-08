#!/usr/bin/env bash
# Drive a Game & Watch over SWD from the machine the probe is plugged into.
#
# Why this exists: every measurement so far has cost a build, a release, a flash
# and a human reading a number off the screen -- ten minutes to answer one
# question, which is why questions like "Mario Kart is at 34 fps, so why is it
# asleep 8% of the time" got guessed at instead of measured. The host harness
# cannot see the answer (no interrupts, no cache) and neither can the QEMU rig
# (no cache, no bus contention). Only the device knows, and this is how to ask.
#
#   gnw_probe.sh detect            which probe is attached, and does SWD answer
#   gnw_probe.sh flash <file.bin>  flash an intflash image via gnwmanager
#   gnw_probe.sh sample <elf> [n]  statistical profile: halt, read PC, resume,
#                                  n times, then a histogram by symbol
#   gnw_probe.sh gdb <elf>         interactive gdb against the running device
#
# Probe selection, in order: an explicit $IFACE, an attached ST-Link, an
# attached CMSIS-DAP (a Raspberry Pi Pico running debugprobe is one), then the
# Pi 5's own GPIO (three wires: SWDIO, SWCLK, GND -- no probe hardware at all).
set -euo pipefail

OPENOCD=${OPENOCD:-openocd}
GDB=${GDB:-gdb-multiarch}
GNWMANAGER=${GNWMANAGER:-$HOME/.local/bin/gnwmanager}
PORT=${PORT:-3333}
TARGET=${TARGET:-target/stm32h7x.cfg}

pick_iface() {
  if [ -n "${IFACE:-}" ]; then echo "$IFACE"; return; fi
  if lsusb 2>/dev/null | grep -qiE 'st-link|stlink'; then echo interface/stlink-dap.cfg; return; fi
  if lsusb 2>/dev/null | grep -qiE 'cmsis-dap|picoprobe|debugprobe'; then echo interface/cmsis-dap.cfg; return; fi
  if [ -e /dev/gpiochip0 ]; then echo interface/raspberrypi5-gpiod.cfg; return; fi
  echo "no probe found: attach an ST-Link or a Pico (debugprobe), or wire" >&2
  echo "SWDIO/SWCLK/GND to the Pi 5 header and make sure /dev/gpiochip0 exists" >&2
  exit 1
}

start_openocd() {   # leaves OpenOCD running in the background, returns its pid
  local iface; iface=$(pick_iface)
  echo "[probe] interface: $iface" >&2
  sudo "$OPENOCD" -f "$iface" -f "$TARGET" \
       -c "adapter speed 4000" -c "gdb_port $PORT" >/tmp/gnw_openocd.log 2>&1 &
  local pid=$!
  for _ in $(seq 1 30); do
    grep -q "Listening on port $PORT" /tmp/gnw_openocd.log 2>/dev/null && { echo "$pid"; return; }
    kill -0 "$pid" 2>/dev/null || { echo "[probe] OpenOCD died:" >&2; tail -15 /tmp/gnw_openocd.log >&2; exit 1; }
    sleep 0.3
  done
  echo "[probe] OpenOCD did not open the gdb port:" >&2; tail -15 /tmp/gnw_openocd.log >&2
  kill "$pid" 2>/dev/null || true; exit 1
}

case "${1:-}" in
  detect)
    echo "--- USB ---"; lsusb 2>/dev/null | grep -iE 'st-link|stlink|cmsis|pico|debugprobe' || echo "(no USB probe)"
    echo "--- GPIO ---"; [ -e /dev/gpiochip0 ] && echo "/dev/gpiochip0 present (Pi native SWD possible)" || echo "(none)"
    pid=$(start_openocd)
    sleep 1
    grep -iE "cortex|target halted|Info : STM32|IDCODE|error" /tmp/gnw_openocd.log | head -10
    kill "$pid" 2>/dev/null || true
    ;;

  flash)
    bin=${2:?usage: gnw_probe.sh flash <intflash.bin>}
    "$GNWMANAGER" flash bank1 "$bin" -- start
    ;;

  # Statistical profiler. No instrumentation in the firmware, no reflash per
  # question: halt, read $pc, resume, repeat. The distribution of PCs IS the
  # profile, and unlike the rigs it includes cache stalls and interrupt time,
  # because it is the real chip.
  #
  # The loop runs inside OpenOCD rather than through gdb: a gdb batch script
  # driving interrupt/continue is asynchronous and drops most of its samples --
  # it produced zero on the first try. OpenOCD's own halt/resume is synchronous.
  sample)
    elf=${2:?usage: gnw_probe.sh sample <elf> [samples]}
    n=${3:-2000}
    iface=$(pick_iface)
    echo "[probe] interface: $iface, $n samples" >&2
    sudo "$OPENOCD" -f "$iface" -f "$TARGET" -c "adapter speed 4000" \
      -c "init" \
      -c "for {set i 0} {\$i < $n} {incr i} { halt; echo [reg pc -force]; resume; }" \
      -c "shutdown" > /tmp/gnw_sample.log 2>&1 || true
    # OpenOCD prints "pc (/32): 0x08012345"; take those lines only, so nothing
    # else in the log can be mistaken for a sample.
    grep -oE '^pc \(/32\): 0x[0-9a-f]+' /tmp/gnw_sample.log | awk '{print $3}' > /tmp/gnw_pcs.txt || true
    cnt=$(wc -l < /tmp/gnw_pcs.txt)
    echo "[probe] $cnt samples"
    [ "$cnt" -gt 0 ] || { echo "[probe] no samples -- OpenOCD said:"; tail -12 /tmp/gnw_sample.log; exit 1; }
    sort /tmp/gnw_pcs.txt | uniq -c | sort -rn | head -30 | while read -r c addr; do
      sym=$("$GDB" -q -batch -ex "info symbol $addr" "$elf" 2>/dev/null | head -1)
      printf "%6d  %5.1f%%  %-12s %s\n" "$c" "$(awk -v a="$c" -v b="$cnt" 'BEGIN{printf "%.1f", 100*a/b}')" "$addr" "$sym"
    done
    ;;

  # Inject a button press without touching the hardware. buttons_get() reads
  # the GPIOs and returns a mask in r0, so: break at its entry, set r0 to the
  # mask we want and pc to lr -- the function never runs, the caller gets our
  # answer. This is how the device gets driven from here at all; there is no
  # one at the console to press anything.
  #
  #   press <mask-hex> <halts>    B_Left 1 B_Up 2 B_Right 4 B_Down 8
  #                               B_A 10 B_B 20 B_TIME 40 B_GAME 80
  press)
    mask=${2:?usage: gnw_probe.sh press <mask-hex> [halts]}
    halts=${3:-40}
    iface=$(pick_iface)
    entry=$(${GDB} -q -batch -ex "info address buttons_get" "${ELF:-/tmp/gw.elf}" 2>/dev/null \
            | grep -oE '0x[0-9a-f]+' | head -1)
    entry=${entry:-0x08102f74}
    echo "[probe] buttons_get at $entry, injecting mask $mask for $halts polls" >&2
    sudo "$OPENOCD" -f "$iface" -f "$TARGET" -c "adapter speed 4000" \
      -c "init" -c "reset run" \
      -c "bp $entry 2 hw" \
      -c "for {set i 0} {\$i < $halts} {incr i} { wait_halt 2000; reg r0 $mask; set s [reg lr -force]; regexp {0x[0-9a-f]+} \$s hex; reg pc \$hex; resume }" \
      -c "rbp $entry" -c "resume" -c "shutdown" > /tmp/gnw_press.log 2>&1 || true
    echo "[probe] breakpoint hits: $(grep -c 'halted due to breakpoint' /tmp/gnw_press.log)"
    ;;

  gdb)
    elf=${2:?usage: gnw_probe.sh gdb <elf>}
    pid=$(start_openocd)
    trap 'kill "$pid" 2>/dev/null || true' EXIT
    "$GDB" "$elf" -ex "target extended-remote :$PORT"
    ;;

  *)
    sed -n '2,20p' "$0"; exit 1;;
esac
