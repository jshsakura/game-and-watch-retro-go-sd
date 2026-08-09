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

# The canonical release flag set (.github/workflows/package.yml), plus the two
# measurement-only knobs: the PPU line cache the device already voted for, and
# an autoboot so a benchmark needs nobody at the console.
BASE_FLAGS=(
  CHECK_DIRTY_SUBMODULE=0 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1
  DISABLE_SPLASH_SCREEN=1 ENABLE_BOOT_OC=1 INTFLASH_BANK=2 CHEAT_CODES=1
  ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1 SNES_LINE_CACHE=1
  "GNW_AUTOBOOT=${GNW_AUTOBOOT:-0}"
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
  echo "[arm:$name] intflash $(stat -c%s "$ARMS/$name/gw_retro_go_intflash.bin") B, snes.bin $(stat -c%s "$ARMS/$name/snes.bin") B"
}

cmd_flash() {
  local name=$1
  local d="$ARMS/$name"
  [ -f "$d/gw_retro_go_intflash.bin" ] || { echo "no such arm: $name"; exit 1; }
  scp -q "$d/gw_retro_go_intflash.bin" "$d/snes.bin" "$HOST:/tmp/"
  ssh "$HOST" "python3 -m gnwmanager flash $INTFLASH_ADDR /tmp/gw_retro_go_intflash.bin \
      -- sdpush --file /tmp/snes.bin --dest-path '/cores/' \
      -- start $INTFLASH_ADDR" 2>&1 | tail -3
  echo "[arm:$name] flashed"
}

cmd_bench() {
  local name=$1; local frames=${2:-1800}
  for i in 1 2 3; do
    bash tools/gnw_probe/bench.sh "$ARMS/$name/gw_retro_go.elf" "$frames"
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
