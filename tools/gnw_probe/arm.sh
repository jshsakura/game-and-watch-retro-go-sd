#!/usr/bin/env bash
# Build one A/B arm the way the release builds it, park its artifacts under a
# name, flash it, and hand the ELF to bench.sh.
#
#   arm.sh build <name> [extra make vars...]
#   arm.sh flash <name>
#   arm.sh bench <name> [frames]
#   arm.sh run   <name> [extra make vars...]     # build + flash + 3x bench
#
# Why a script and not three commands typed by hand: the SNES core is an
# overlay that lives on the SD card, not in the internal flash image. Flashing
# gw_retro_go_intflash.bin alone leaves /cores/snes.bin from the PREVIOUS arm
# on the card, so the device keeps running the old interpreter while the
# benchmark reports on the new one. Every arm therefore pushes snes.bin too.
#
# The compiler is the container's (gcc 15.2). The host's 13.2 overflows the
# 256 KB bank by 11 KB with the release locales, and -- more importantly --
# it is not the compiler that produces the number anyone ships.
set -euo pipefail
cd "$(dirname "$0")/../.."

HOST=${PROBE_HOST:-rpi-genie5}
ARMS=${ARMS:-/tmp/gnw_arms}
INTFLASH_ADDR=0x08100000

# The canonical release flag set (.github/workflows/package.yml), plus the
# measurement-only knobs: an autoboot so a benchmark needs nobody at the console,
# and the idle timeout compiled out so the console does not power itself off
# mid-session. The PPU line cache used to be forced on here; it is off now, by
# measurement in the scene the game is played in.
BASE_FLAGS=(
  CHECK_DIRTY_SUBMODULE=0 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1
  DISABLE_SPLASH_SCREEN=1 ENABLE_BOOT_OC=1 INTFLASH_BANK=2 CHEAT_CODES=1
  ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1
  "GNW_AUTOBOOT=${GNW_AUTOBOOT:-0}"
  # A benchmark has nobody at the console, and the idle timeout powers the
  # device off mid-session. Measurement arms always keep it awake.
  GNW_NO_IDLE_OFF=1
  # An ablation arm that hangs is a failed boot; two of those and the rescue
  # screen stops every later flash for a button nobody is there to press.
  GNW_NO_BOOT_RESCUE=1
)

in_container() {
  docker run --rm --init --network host \
    --name "retro-go-sd-arm-$$" \
    -e GIT_TAG_OVERRIDE="$(git describe --tags --dirty=+ 2>/dev/null)" \
    --user "$(id -u):$(id -g)" -v "$PWD:/opt/workdir" \
    sylverb/retro-go-sd-builder:v1.5 "$@"
}

cmd_build() {
  local name=$1; shift
  echo "[arm:$name] build ${*:-(no extra flags)}"
  in_container make -j"$(nproc)" all "${BASE_FLAGS[@]}" "$@" >"$ARMS/$name.build.log" 2>&1 \
    || { tail -25 "$ARMS/$name.build.log"; exit 1; }
  in_container make create_sd_data "${BASE_FLAGS[@]}" "$@" >>"$ARMS/$name.build.log" 2>&1 \
    || { tail -25 "$ARMS/$name.build.log"; exit 1; }
  mkdir -p "$ARMS/$name"
  cp build/gw_retro_go.elf build/gw_retro_go_intflash.bin sd_content/cores/snes.bin "$ARMS/$name/"
  # The native ports ship as homebrew PACKAGES, and a package carries veneer
  # literals holding the resident addresses of the firmware that built it. Put
  # a package from one build on a console running another and it branches into
  # whatever is at the old address -- Super Metroid took a UsageFault jumping to
  # 0x200025f8, which is the FatFs object. Keep them per arm so flashing an arm
  # can push a matching set.
  for f in "sd_content/roms/homebrew/Super Metroid.bin" sd_content/roms/homebrew/sm.xip; do
    [ -f "$f" ] && cp "$f" "$ARMS/$name/"
  done
  # Every core's overlay, not just the SNES one. A core .bin built by one
  # firmware and loaded by another is a different program: the SM port jumped
  # into the FatFs object and UsageFaulted, and Virtual Boy sat in a message box
  # with its frame counter at zero -- which looked exactly like "this build
  # breaks VB" and was measured as such twice today, by two sessions.
  mkdir -p "$ARMS/$name/cores"
  cp sd_content/cores/*.bin "$ARMS/$name/cores/" 2>/dev/null || true
  echo "[arm:$name] intflash $(stat -c%s "$ARMS/$name/gw_retro_go_intflash.bin") B, snes.bin $(stat -c%s "$ARMS/$name/snes.bin") B"
}

cmd_flash() {
  local name=$1
  local d="$ARMS/$name"
  [ -f "$d/gw_retro_go_intflash.bin" ] || { echo "no such arm: $name"; exit 1; }
  scp -q "$d/gw_retro_go_intflash.bin" "$d/snes.bin" "$HOST:/tmp/"
  # Push this arm's homebrew packages too when it has them, for the reason in
  # cmd_build. PUSH_HB=0 skips it (it costs a few seconds).
  # PUSH_CORE=<name[,name...]> pushes those cores' overlays from THIS arm, e.g.
  # PUSH_CORE=vb. Pushing all of them every flash costs minutes; pushing none is
  # how the mismatch above keeps happening. Name the one you are measuring.
  if [ -n "${PUSH_CORE:-}" ]; then
    for c in ${PUSH_CORE//,/ }; do
      [ -f "$d/cores/$c.bin" ] || { echo "[arm:$name] no cores/$c.bin in this arm" >&2; continue; }
      scp -q "$d/cores/$c.bin" "$HOST:/tmp/core_$c.bin"
      ssh -n "$HOST" "python3 -m gnwmanager sdpush --file /tmp/core_$c.bin --dest-path '/cores/$c.bin'" \
        >/dev/null 2>&1 && echo "[arm:$name] pushed cores/$c.bin"
    done
  fi
  if [ "${PUSH_HB:-1}" = 1 ] && [ -f "$d/Super Metroid.bin" ]; then
    scp -q "$d/Super Metroid.bin" "$HOST:/tmp/sm_port.bin"
    [ -f "$d/sm.xip" ] && scp -q "$d/sm.xip" "$HOST:/tmp/sm.xip"
    ssh -n "$HOST" "python3 -m gnwmanager sdpush --file /tmp/sm_port.bin --dest-path '/roms/homebrew/Super Metroid.bin' \
        -- sdpush --file /tmp/sm.xip --dest-path '/roms/homebrew/'" >/dev/null 2>&1 || true
  fi
  ssh "$HOST" "python3 -m gnwmanager flash $INTFLASH_ADDR /tmp/gw_retro_go_intflash.bin \
      -- sdpush --file /tmp/snes.bin --dest-path '/cores/' \
      -- start $INTFLASH_ADDR" 2>&1 | tail -3
  echo "[arm:$name] flashed"
}

# An arm that did not resume its savestate is looking at a different scene, and
# a cheaper one. That is not a hypothetical: it read +10.5 drawn fps once and
# survived three bracketed runs before anyone noticed. Any struct change alters
# the state's payload length and the loader refuses it, correctly and silently.
cmd_bench() {
  local name=$1; local frames=${2:-1800}
  local elf="$ARMS/$name/gw_retro_go.elf"
  local sym
  sym=$(arm-none-eabi-nm "$elf" 2>/dev/null | awk '$3 ~ /g_snes_state_resumed$/ {print "0x"$1; exit}')
  if [ -n "$sym" ]; then
    local v
    v=$(ssh "$HOST" "sudo openocd -f interface/stlink-dap.cfg -f target/stm32h7x.cfg         -c 'adapter speed 4000' -c init -c 'mdw $sym' -c shutdown 2>&1"         | grep -oE ': [0-9a-f]{8}' | tr -d ': ')
    if [ "$v" = "00000000" ]; then
      echo "[arm:$name] SAVESTATE REFUSED -- this arm booted COLD and is not" >&2
      echo "            measuring the same scene. Numbers from it mean nothing." >&2
      return 4
    fi
  fi
  for i in 1 2 3; do
    bash tools/gnw_probe/bench.sh "$elf" "$frames"
  done
}

mkdir -p "$ARMS"
case "${1:-}" in
  build) shift; cmd_build "$@" ;;
  flash) shift; cmd_flash "$@" ;;
  bench) shift; cmd_bench "$@" ;;
  run)   name=$1; shift; cmd_build "$name" "$@"; cmd_flash "$name"; sleep 3; cmd_bench "$name" ;;
  *) sed -n '2,18p' "$0"; exit 1 ;;
esac
