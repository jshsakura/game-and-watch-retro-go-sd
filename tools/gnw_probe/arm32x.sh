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
  # …and again under cores/, which is where drawn_ab.sh looks.
  #
  # It looked there and did not find it, so `CORE=32x` printed "no cores/32x.bin
  # in this arm -- SKIPPED" and every 32X measurement ran with its card-side
  # check disabled. That check is not decoration here: a core-local knob
  # (MD32X_OC_LEVEL, MD32X_FORCED_DRAW_RATIO, any MD32X_C_DEFS entry) leaves the
  # two arms' intflash BYTE-IDENTICAL, so the flash-side check passes for either
  # arm and the card's 32x.bin is the only thing that says which program ran.
  # Skipping it left the A/B with no way at all to detect measuring the wrong
  # arm -- the failure this whole directory keeps paying for, three times in one
  # day. It skipped loudly, which is the rule, and the loud line still went by
  # unread for a full bench.
  mkdir -p "$ARMS/$name/cores"
  cp sd_content/cores/32x.bin sd_content/cores/32x.xip "$ARMS/$name/cores/"
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
  for other in "$ARMS"/*/gw_retro_go_intflash.bin; do
    [ -e "$other" ] || continue
    o=$(dirname "$other"); [ "$(basename "$o")" = "$name" ] && continue
    if cmp -s "$o/32x.bin" "$ARMS/$name/32x.bin" &&
       cmp -s "$o/gw_retro_go_intflash.bin" "$ARMS/$name/gw_retro_go_intflash.bin"; then
      # A warning watched this class for one session and the warning went by
      # unread twice (rule-every-recipe-define-group-belongs-in-flags-stamp;
      # GNW_STOP2_PLAIN wired into MD32X_C_DEFS while gw_sleep.c is main
      # firmware and never sees it -- two arms, one program, "no effect").
      # An A/B pair that is byte-identical is not a smaller experiment, it is
      # the same experiment twice. Refuse, exactly like verify_intflash.
      echo "[32x:$name] REFUSING: byte-identical to arm '$(basename "$o")'." >&2
      echo "  A knob did not reach the compiler. Check the define group:" >&2
      echo "  core-local knobs ride MD32X_C_DEFS, main-firmware knobs (gw_sleep.c," >&2
      echo "  rg_alarm.c) ride C_DEFS. And check the knob's guard is not nested" >&2
      echo "  inside another knob's ifneq." >&2
      exit 1
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
  verify_intflash "$d/gw_retro_go_intflash.bin"
  echo "[32x:$name] flashed"
}

# The card's core bin is only half the program -- the other half is the
# intflash image, and until 2026-08-26 nothing checked it. That night base4
# failed to boot on 6/6 runs while its card md5 verified clean, and the only
# unverified variable was the intflash transfer (suspected corruption; the
# device wedged before it could be confirmed). So: read the flash back and
# compare byte-for-byte. A mismatch REFUSES the flash (exit 1), not a warning
# -- a half-verified bench is worse than no bench.
verify_intflash() {
  local expect=$1
  local sz; sz=$(stat -c%s "$expect")
  local got_md5 exp_md5
  exp_md5=$(md5sum "$expect" | awk '{print $1}')
  timeout 60 openocd -c 'adapter speed 4000' -f interface/stlink-dap.cfg \
      -f target/stm32h7x.cfg -c init \
      -c "dump_image /tmp/intflash_read.bin $INTFLASH_ADDR $sz" \
      -c shutdown >/dev/null 2>&1
  if [ ! -s /tmp/intflash_read.bin ] || [ "$(stat -c%s /tmp/intflash_read.bin)" != "$sz" ]; then
    echo "[32x] INTFLASH VERIFY FAILED: read-back did not complete ($sz B expected) -- REFUSING"; exit 1
  fi
  got_md5=$(md5sum /tmp/intflash_read.bin | awk '{print $1}')
  if [ "$got_md5" != "$exp_md5" ]; then
    echo "[32x] INTFLASH MISMATCH: device $got_md5 != arm $exp_md5 -- REFUSING (flash transfer corrupt)"; exit 1
  fi
  echo "[32x] intflash verified $exp_md5 ($sz B)"
}

# ---------------------------------------------------------------------------
# Anchor-state gate (trap 64: "the bench's anchor savestate is part of the
# program too"). The device bench chain consumes /data/off.sav (empty =>
# hibernate resume refused => boot-attract workload) and every anchored rig
# measurement consumes slot0. On 2026-08-28 slot0 had silently become an
# MD-era save (32X never started, 141187B md5 c824ddbe) and a block-trace run
# measured a frankenstate for half a day before the cross-check caught it.
# Both files are hashed before every bench and compared against the registry
# below. Mismatch = REFUSE (a warning would teach people to ignore it).
#
# The slot0 registry below is deliberately the PENDING capture target, not
# the invalid file currently on the card -- so every bench refuses until the
# real 32X-active gameplay save is captured (user plays a little, saves
# in-game; leader m4194) and its size+md5 are recorded here. off.sav's empty
# state is the genuine expected value (empty => resume refused => the bench
# measures boot-attract, which is what every fps number so far has measured).
#
# Escape hatch (emergencies only, prints a loud warning): ANCHOR_CHECK=0
# ---------------------------------------------------------------------------
verify_anchor() {
  [ "${ANCHOR_CHECK:-1}" = "1" ] || { echo "[32x] ANCHOR GATE BYPASSED (ANCHOR_CHECK=0) -- numbers are not anchored, label them accordingly"; return 0; }
  # NB: this file runs `set -euo pipefail`; every best-effort probe below is
  # individually guarded so a flaky pull degrades into a REFUSE, never into a
  # silent script death.
  timeout 12 openocd -c 'adapter speed 4000' -f interface/stlink-dap.cfg \
    -f target/stm32h7x.cfg -c init -c 'reset halt' -c shutdown >/dev/null 2>&1 || true
  ssh "$HOST" "python3 -m gnwmanager sdpull '/data/32x/둠 (Doom).32x-0.sav' /tmp/anch0.bin" >/dev/null 2>&1 || true
  ssh "$HOST" "python3 -m gnwmanager sdpull /data/off.sav /tmp/anchoff.bin" >/dev/null 2>&1 || true
  scp -q "$HOST:/tmp/anch0.bin" /tmp/anch0.bin 2>/dev/null || true
  scp -q "$HOST:/tmp/anchoff.bin" /tmp/anchoff.bin 2>/dev/null || true
  timeout 12 openocd -c 'adapter speed 4000' -f interface/stlink-dap.cfg \
    -f target/stm32h7x.cfg -c init -c 'reset run' -c shutdown >/dev/null 2>&1 || true
  local bad=0 what esz emd5 path got_sz got_md5
  for what in slot0 off.sav; do
    case $what in
      slot0)  esz=$ANCHOR_SLOT0_SIZE; emd5=$ANCHOR_SLOT0_MD5; path=/tmp/anch0.bin ;;
      off.sav) esz=$ANCHOR_OFF_SIZE;  emd5=$ANCHOR_OFF_MD5;  path=/tmp/anchoff.bin ;;
    esac
    got_sz=$(stat -c%s "$path" 2>/dev/null || echo -1)
    got_md5=$(md5sum "$path" 2>/dev/null | awk '{print $1}' || echo none)
    if [ "$got_sz" != "$esz" ] || [ "$got_md5" != "$emd5" ]; then
      echo "[32x] ANCHOR MISMATCH $what: device $got_sz B/$got_md5 != registry $esz B/$emd5 -- REFUSING" >&2
      echo "[32x] anchor registry describes: $ANCHOR_DESC" >&2
      bad=1
    fi
  done
  [ $bad -eq 0 ] && echo "[32x] anchor verified: $ANCHOR_DESC" || exit 1
}

# Registry (update on anchor capture; description is one line, it travels with the numbers):
ANCHOR_SLOT0_SIZE=678925                              # PENDING CAPTURE -- 32X-active gameplay save, user in-game save
ANCHOR_SLOT0_MD5=PENDING-CAPTURE                      # PENDING CAPTURE -- record md5 when the save lands
ANCHOR_OFF_SIZE=0
ANCHOR_OFF_MD5=d41d8cd98f00b204e9800998ecf8427e       # empty => hibernate refused => boot-attract
ANCHOR_DESC="PENDING: 32X-active gameplay save (target ~678925B v3); device currently holds INVALID MD-era 141187B"

# drawn_ab.sh already falls back to Core/Src/porting/common.c's shared counters
# when the SNES symbols are absent, which is exactly this core's case.
cmd_bench() {
  local name=$1; local frames=${2:-900}
  verify_anchor
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
