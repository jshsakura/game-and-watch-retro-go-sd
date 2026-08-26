#!/usr/bin/env bash
# powercycle.sh -- verified power-cycle procedure for the first boot after an
# SWD flash, extracted from arm32x.sh 2026-08-25 after the inline hook died of
# SIGPIPE (exit 141, no output) inside that tool environment. Foreground runs
# work; the sequence below was verified end-to-end manually (alarm -> STANDBY
# -> alarm wake -> cold boot -> DOOM autoboot, 2026-08-25).
#
# WHY THIS EXISTS: every boot hang observed on this device (3 total: tl2oc2,
# triple, and one on a lever-free base0) was the FIRST boot after an SWD
# flash. Plain resets went 0/40 clean and power-cycle boots 0/9 clean, so the
# untested variable is whatever controller state flashing leaves behind.
# STANDBY drops the 3V3 rail, which cold-starts the QSPI/SD controllers the
# way a battery pull would (VBAT and the backup domain do survive -- that gap
# stays untested until someone opens the case). The alarm keeps it
# unattended. GW_EnterDeepSleep re-reads /clock/clock.cfg on entry, so the
# alarm pushed here is the one that wakes it.
#
# BENCH PROCEDURE (downgraded 2026-08-25 per the leader's call): arm32x.sh no
# longer calls this automatically -- a half-running gate is worse than none.
# After `arm32x.sh flash <arm>`, run:
#     tools/gnw_probe/powercycle.sh /tmp/gnw_arms/<arm>/gw_retro_go.elf
# and treat a reported INCOMPLETE the way the hook used to: re-run it, or
# treat the next boot's numbers with suspicion.
#
# Usage: powercycle.sh <elf>
set -u
# the pre-2026-08-25 ten-second behavior for scratch runs.
powercycle_after_flash() {
  local elf=$1
  local ocdb=(-c 'adapter speed 4000' -f interface/stlink-dap.cfg -f target/stm32h7x.cfg)
  ocd() { timeout 12 openocd "${ocdb[@]}" -c init -c "$1" -c shutdown 2>&1; }
  bail() {
    echo "[32x] POWERCYCLE INCOMPLETE: $1 -- falling back to plain reset." \
         "The next boot is the kind that hung 3 times; treat its numbers"  >&2
    echo "        with suspicion or re-run the flash."                      >&2
    pkill -x openocd 2>/dev/null
    ocd 'reset run' >/dev/null 2>&1 || true
    return 0
  }
  [ -f "$elf" ] || { echo "[32x] POWERCYCLE SKIP: no ELF in this arm" >&2; return 0; }

  # The emu-frame counter address is TREE-DEPENDENT: the crumb wiring moved it
  # 0x20002404 -> 0x2000242C, and the hardcoded reads below made every check
  # read dead BSS -- the hook then bailed INCOMPLETE on every flash while the
  # power cycles were probably succeeding (2026-08-25, all three sandwiches).
  # Resolve it from the ELF we just flashed, like drawn_ab.sh does.
  local EMU_A
  EMU_A=$(arm-none-eabi-nm "$elf" 2>/dev/null | awk '$3=="g_common_emu_frames"{print $1; exit}')
  case "$EMU_A" in
    [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ;;
    *) echo "[32x] POWERCYCLE SKIP: g_common_emu_frames unresolved in $elf --" \
             "do not trust flash-adjacent boots from this run" >&2; return 0 ;;
  esac

  # RTC time (BCD) -> an alarm minute at least 90 s out.
  local tr
  tr=$(ocd 'mdw 0x58004000' | grep -aoE '0x58004000: [0-9a-f]+' | grep -aoE '[0-9a-f]+$')
  [ -n "$tr" ] || bail "RTC unreadable"
  eval "$(python3 - "$tr" <<'PYEOF'
import sys
t = int(sys.argv[1], 16)
now = (((t>>20)&3)*10+((t>>16)&0xf))*3600 + (((t>>12)&7)*10+((t>>8)&0xf))*60 + ((t>>4)&7)*10+(t&0xf)
tgt = ((now + 150) // 60) * 60 % 86400
d = (tgt - now) % 86400
if d < 90: tgt = (tgt + 60) % 86400; d += 60
print(f"PC_ALARM={tgt//3600:02d}{(tgt%3600)//60:02d}")
print(f"PC_WAIT={d}")
PYEOF
)"
  [ -n "${PC_ALARM:-}" ] || bail "RTC decode failed"

  # Rewrite /clock/clock.cfg with that alarm, keeping whatever else is there.
  python3 -m gnwmanager sdpull /clock/clock.cfg /tmp/pc_clock.cfg >/dev/null 2>&1 || true
  { [ -s /tmp/pc_clock.cfg ] && grep -av '^alarm=' /tmp/pc_clock.cfg
    printf 'alarm=%s,1\n' "$PC_ALARM"; } > /tmp/pc_clock_new.cfg
  ocd 'reset halt' >/dev/null || bail "reset halt failed"
  python3 -m gnwmanager sdpush /tmp/pc_clock_new.cfg /clock/clock.cfg >/dev/null 2>&1 \
    || bail "cfg sdpush failed"

restore_clock_cfg() {
  # The alarm we push is DAILY: leaving it behind re-fires every day and its
  # in-game banner freezes the emu counter -- the "sporadic defect" signature
  # found 2026-08-26. Put back what was there before we touched it.
  if [ -s /tmp/pc_clock.cfg ]; then
    ocd 'reset halt' >/dev/null 2>&1 || true
    python3 -m gnwmanager sdpush /tmp/pc_clock.cfg /clock/clock.cfg >/dev/null 2>&1 \
      || echo "[32x] WARNING: clock.cfg restore failed -- check /clock/clock.cfg for a leftover alarm= line" >&2
    ocd 'reset run' >/dev/null 2>&1 || true
  fi
}
# Third alarm-residue incident (2026-08-26): a killed run left alarm= behind
# because restore only ran on explicit paths. Every exit now restores --
# the original clock.cfg is saved before we touch it, and the trap fires
# on TERM/INT/EXIT alike, so a watchdog-kill or Ctrl-C cannot strand a
# daily alarm on the user's card. Placed after the definition so an early
# set -u exit never references an unset function.
trap restore_clock_cfg EXIT INT TERM

  # Boot into the game, then drop to standby from inside it (the function
  # re-arms the RTC alarm from the cfg we just pushed).
  ocd 'reset run' >/dev/null || bail "reset run failed"
  local i emu=0 hex
  for i in $(seq 1 24); do
    sleep 5
    hex=$(ocd "mdw 0x$EMU_A" | grep -aoE "0x$EMU_A: [0-9a-f]+" | grep -aoE '[0-9a-f]+$')
    [ -n "$hex" ] && emu=$((16#$hex)) && [ "$emu" -gt 100 ] && break
  done
  [ "${emu:-0}" -gt 100 ] || bail "no autoboot (emu=$emu)"
  ocd 'halt' >/dev/null || bail "halt failed"
  pkill -x openocd 2>/dev/null; sleep 1
  nohup python3 -m gnwmanager gdbserver >/tmp/pc_gdbsrv.log 2>&1 &
  local gsrv=$!
  sleep 4
  timeout 40 gdb-multiarch "$elf" -batch \
    -ex 'target extended-remote :3333' \
    -ex 'set unwind-on-signal on' \
    -ex 'call (void)GW_EnterDeepSleep(1, 0, 0)' >/dev/null 2>&1
  kill "$gsrv" 2>/dev/null; pkill -x openocd 2>/dev/null; sleep 2

  # STANDBY latching = the DAP loses power. If the chip answered, the call
  # fell back to a SystemReset instead and this is NOT a power cycle.
  local probe
  for i in 1 2 3 4 5; do
    probe=$(timeout 10 openocd "${ocdb[@]}" -c init -c "mdw 0x$EMU_A" -c shutdown 2>&1 \
      | grep -c "0x$EMU_A") || probe=0
    if [ "$probe" -gt 0 ]; then
      restore_clock_cfg
      bail "STANDBY did not latch (SystemReset fallback)"
    fi
    sleep 2
  done

  # Alarm minute + margin, then the DAP comes back with the cold boot.
  sleep $((PC_WAIT + 20))
  local up=0
  for i in $(seq 1 12); do
    probe=$(timeout 10 openocd "${ocdb[@]}" -c init -c "mdw 0x$EMU_A" -c shutdown 2>&1 \
      | grep -c "0x$EMU_A") || probe=0
    [ "$probe" -gt 0 ] && { up=1; break; }
    sleep 5
  done
  [ "$up" -eq 1 ] || { restore_clock_cfg; bail "no alarm wake (device dark: press POWER once)"; }
  emu=0
  for i in $(seq 1 18); do
    sleep 5
    hex=$(ocd "mdw 0x$EMU_A" | grep -aoE "0x$EMU_A: [0-9a-f]+" | grep -aoE '[0-9a-f]+$')
    [ -n "$hex" ] && emu=$((16#$hex)) && [ "$emu" -gt 100 ] && break
  done
  [ "${emu:-0}" -gt 100 ] || { restore_clock_cfg; bail "post-cycle boot stalled (emu=$emu)"; }
  restore_clock_cfg
  echo "[32x] power-cycled: STANDBY + alarm wake, cold boot clean (emu=$emu)"
}
powercycle_after_flash "$1"
