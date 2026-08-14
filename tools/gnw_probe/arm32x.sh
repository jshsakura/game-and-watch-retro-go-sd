#!/usr/bin/env bash
# Build, flash and bench one Sega 32X arm — arm.sh's sibling, for the core that
# was removed in July and restored to be measured with instruments that did not
# exist then.
#
#   arm32x.sh build <name> [extra make vars...]
#   arm32x.sh rom   <rom.32x>          # push a cartridge to /roms/32x/ once
#   arm32x.sh pick  <32x:0 | 32x:Name> # what the autoboot should launch
#   arm32x.sh flash <name>
#   arm32x.sh bench <name> [frames]
#   arm32x.sh run   <name> [extra make vars...]
#
# Three things this exists to get right, each of which produced a wrong number
# for the SNES core first:
#
# 1. THE CORE IS ON THE CARD. Flashing gw_retro_go_intflash.bin alone leaves the
#    previous arm's /cores/32x.bin in place, so the device benchmarks the old
#    core while the log reports the new one. Every flash pushes 32x.bin.
# 2. NO PROFILER. MD32X_DEVICE_PROFILE=1 costs ~16 of every 94 cycles an
#    instruction takes (docs/32X_DEVICE_MEASUREMENT_LOG.md §14). Every 32X fps
#    ever published for this project was measured with that tax on. It is not
#    in BASE_FLAGS and must not be added to a build whose fps is quoted.
# 3. NOT THE TITLE SCREEN. The "19.5 fps" that started the July work was a boot
#    anchor — 64 frames from reset, i.e. a still image. Doom's attract demo is
#    real rendering and is deterministic; GNW_AUTOSAVE_FRAME=<n> makes the
#    console save slot 0 once it is in it, and later arms resume there.
set -euo pipefail
cd "$(dirname "$0")/../.."

HOST=${PROBE_HOST:-rpi-genie5}
ARMS=${ARMS:-/tmp/gnw_arms}
INTFLASH_ADDR=0x08100000

# The canonical release flag set (.github/workflows/package.yml) plus the
# measurement-only knobs. GNW_AUTOBOOT=0 compiles in the autoboot; the ROM it
# actually launches comes from /snes_bench_index.txt on the card ("pick").
BASE_FLAGS=(
  CHECK_DIRTY_SUBMODULE=0 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1
  DISABLE_SPLASH_SCREEN=1 ENABLE_BOOT_OC=1 INTFLASH_BANK=2 CHEAT_CODES=1
  ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1
  "GNW_AUTOBOOT=${GNW_AUTOBOOT:-0}"
  GNW_NO_IDLE_OFF=1
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
  # build/ is shared with every other session in this checkout.
  if [ -n "$(docker ps --filter name=retro-go-sd-arm -q)" ]; then
    echo "another arm build is running -- build/ is shared, refusing" >&2; exit 3
  fi
  echo "[32x:$name] build ${*:-(no extra flags)}"
  in_container make -j"$(nproc)" all "${BASE_FLAGS[@]}" "$@" >"$ARMS/$name.build.log" 2>&1 \
    || { tail -25 "$ARMS/$name.build.log"; exit 1; }
  in_container make create_sd_data "${BASE_FLAGS[@]}" "$@" >>"$ARMS/$name.build.log" 2>&1 \
    || { tail -25 "$ARMS/$name.build.log"; exit 1; }
  mkdir -p "$ARMS/$name"
  # BOTH halves of the core. 32x.xip is 972 KB of cold code and rodata that
  # lives in external flash (Cz80_Exec, the YM tables), and the RAM overlay
  # branches into it by address -- push one without the other and the device
  # runs half of each arm.
  cp build/gw_retro_go.elf build/gw_retro_go_intflash.bin \
     sd_content/cores/32x.bin sd_content/cores/32x.xip "$ARMS/$name/"
  echo "[32x:$name] intflash $(stat -c%s "$ARMS/$name/gw_retro_go_intflash.bin") B," \
       "32x.bin $(stat -c%s "$ARMS/$name/32x.bin") B," \
       "32x.xip $(stat -c%s "$ARMS/$name/32x.xip") B"

  # Is this arm a different program from the ones already built?
  #
  # A knob reaches a core only through the define group that core's recipe uses,
  # and 32X has its own (MD32X_C_DEFS) exactly as SNES does. Put a new knob in
  # the wrong group and make is perfectly happy, both arms compile, and the A/B
  # measures one program twice -- which reads as "no effect" and gets written
  # down as a closed path. The SNES session lost an arm to this today; the SNES
  # FLAGS_STAMP bug earlier this month was the same disease.
  for other in "$ARMS"/*/32x.bin; do
    [ -e "$other" ] || continue
    o=$(dirname "$other"); [ "$(basename "$o")" = "$name" ] && continue
    if cmp -s "$o/32x.bin" "$ARMS/$name/32x.bin" &&
       cmp -s "$o/gw_retro_go_intflash.bin" "$ARMS/$name/gw_retro_go_intflash.bin"; then
      echo "[32x:$name] WARNING: byte-identical to arm '$(basename "$o")'." >&2
      echo "            If these were meant to differ, the knob did not reach the" >&2
      echo "            compiler -- check it is in MD32X_C_DEFS, not just C_DEFS." >&2
    fi
  done
}

cmd_rom() {
  local rom=${1:?usage: arm32x.sh rom <rom.32x>}
  scp -q "$rom" "$HOST:/tmp/$(basename "$rom")"
  ssh "$HOST" "python3 -m gnwmanager sdpush --file '/tmp/$(basename "$rom")' --dest-path '/roms/32x/'"
  echo "[32x] pushed $(basename "$rom")"
}

cmd_pick() {
  local sel=${1:?usage: arm32x.sh pick <32x:0|32x:Name>}
  printf '%s\n' "$sel" > /tmp/snes_bench_index.txt
  scp -q /tmp/snes_bench_index.txt "$HOST:/tmp/"
  ssh "$HOST" "python3 -m gnwmanager sdpush --file /tmp/snes_bench_index.txt --dest-path '/'"
  echo "[32x] autoboot -> $sel"
}

cmd_flash() {
  local name=$1
  local d="$ARMS/$name"
  [ -f "$d/gw_retro_go_intflash.bin" ] || { echo "no such arm: $name"; exit 1; }
  scp -q "$d/gw_retro_go_intflash.bin" "$d/32x.bin" "$d/32x.xip" "$HOST:/tmp/"
  ssh "$HOST" "python3 -m gnwmanager flash $INTFLASH_ADDR /tmp/gw_retro_go_intflash.bin \
      -- sdpush --file /tmp/32x.bin --dest-path '/cores/' \
      -- sdpush --file /tmp/32x.xip --dest-path '/cores/' \
      -- start $INTFLASH_ADDR" 2>&1 | tail -3
  echo "[32x:$name] flashed"
}

# drawn_ab.sh already falls back to Core/Src/porting/common.c's shared counters
# when the SNES symbols are absent, which is exactly this core's case.
cmd_bench() {
  local name=$1; local frames=${2:-900}
  for i in 1 2 3; do bash tools/gnw_probe/drawn_ab.sh "$name" "$frames"; done
}

mkdir -p "$ARMS"
case "${1:-}" in
  build) shift; cmd_build "$@" ;;
  rom)   shift; cmd_rom "$@" ;;
  pick)  shift; cmd_pick "$@" ;;
  flash) shift; cmd_flash "$@" ;;
  bench) shift; cmd_bench "$@" ;;
  run)   name=$1; shift; cmd_build "$name" "$@"; cmd_flash "$name"; sleep 3; cmd_bench "$name" ;;
  *) sed -n '2,28p' "$0"; exit 1 ;;
esac
