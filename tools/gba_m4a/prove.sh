#!/usr/bin/env bash
# Is the native M4A block the same program as the one it replaces?
#
# Two questions, and they need two different proofs.
#
#  1. ARITHMETIC — does the native block compute what the interpreted one does?
#     `--blocks` runs EVERY hooked block both ways from the same state over the
#     same memory and compares every register, every flag, every guest cycle and
#     every byte either wrote. Exact, not sampled.
#
#  2. BEHAVIOUR — the native block runs in one go, where the interpreter is cut
#     into slices and interrupted several times along the way. The hardware gets
#     the same cycles either way, but an interrupt that used to land in the middle
#     of the mixer now lands just after it. Whether that is observable is not a
#     question to answer by reasoning: `--e2e` runs the whole game twice, hook off
#     and hook on, and hashes everything the guest can see — screen, audio, IWRAM,
#     EWRAM, VRAM, palette, OAM, I/O and the cycle counter — every frame.
#
# And a RED for each: break the transliteration on purpose, and fail if the test
# does not notice. A test that has never failed proves nothing.
#
#   ./prove.sh <rom.gba> [frames]        both proofs (this is the one to run)
#   ./prove.sh --e2e   <rom.gba> [f]     behaviour only
#   ./prove.sh --blocks <rom.gba> [f]    arithmetic only
#   ./prove.sh --speed <rom.gba> [f]     what it is worth
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
P="$ROOT/external/gpsp"
OUT="${TMPDIR:-/tmp}/gba_m4a_prove"
CC=${CC:-gcc}
CXX=${CXX:-g++}

MODE=all
case "${1:-}" in
    --e2e)    MODE=e2e;    shift ;;
    --blocks) MODE=blocks; shift ;;
    --speed)  MODE=speed;  shift ;;
esac

ROM="${1:-}"
FRAMES="${2:-3000}"

if [ -z "$ROM" ] || [ ! -f "$ROM" ]; then
    echo "usage: $0 [--e2e|--blocks|--speed] <rom.gba> [frames]"
    exit 2
fi
if [ ! -f "$P/cpu.cc" ]; then
    echo "SKIP: external/gpsp not checked out — run git submodule update --init"
    exit 0
fi
cd "$ROOT"

# The device's source set and the device's defines. A prover that links a
# different program than the firmware proves something about a program nobody
# runs — which is how three Super Metroid releases shipped unbootable while the
# harness reported 4,000 clean frames.
CFBASE="-O2 -g -w -fno-strict-aliasing"
CFBASE="$CFBASE -DROM_BUFFER_SIZE=0 -DOBJ_PER_LINE_MAX=32 -DBUFFER_SIZE=4096 -DGBA_SOUND_FREQUENCY=48000"
CFBASE="$CFBASE -I$P -I$P/libretro/libretro-common/include -I$ROOT/tools/gba_harness -I$HERE"

build() {   # build <tag> <defines...>
    local tag="$1"; shift
    local d="$OUT/$tag"
    local CF="$CFBASE $*"
    rm -rf "$d" && mkdir -p "$d"

    $CC  $CF -c "$P/gba_memory.c" -o "$d/gba_memory.o" || return 1
    local f
    for f in sound main savestate input cheats serial serial_proto gbp rfu; do
        $CC $CF -c "$P/$f.c" -o "$d/$f.o" || return 1
    done
    for f in cpu video; do
        $CXX $CF -fno-exceptions -fno-rtti -c "$P/$f.cc" -o "$d/$f.o" || return 1
    done
    $CC $CF -c "$ROOT/Core/Src/porting/gba/gba_frontend.c" -o "$d/fe.o"      || return 1
    $CC $CF -c "$ROOT/tools/gba_harness/host_stubs.c"      -o "$d/hs.o"      || return 1
    $CC $CF -c "$HERE/m4a_hle.c"                           -o "$d/m4a_hle.o" || return 1
    $CC $CF -c "$HERE/m4a_gpsp.c"                          -o "$d/m4a_gpsp.o"|| return 1
    $CC $CF -c "$HERE/prove_main.c"                        -o "$d/prove.o"   || return 1
    $CXX "$d"/*.o -o "$d/run" -lm || return 1
}

rc=0

# ---------------------------------------------------------------- arithmetic
prove_blocks() {
    echo "=== ARITHMETIC: every hooked block, run both ways, compared exactly"
    build blocks "-DGBA_M4A_HLE -DM4A_HLE_VERIFY" || return 1
    "$OUT/blocks/run" "$ROM" "$FRAMES" >/dev/null || return 1

    echo "--- RED: the same run with the transliteration deliberately wrong"
    build blocks_red "-DGBA_M4A_HLE -DM4A_HLE_VERIFY -DM4A_SABOTAGE" || return 1
    if "$OUT/blocks_red/run" "$ROM" "$FRAMES" >/dev/null 2>&1; then
        echo "FAIL: a sabotaged mixer passed the block check. The check proves nothing."
        return 1
    fi
    echo "--- RED confirmed: the sabotaged mixer is caught"
}

# ---------------------------------------------------------------- behaviour
run_hashes() {   # run_hashes <tag> <outfile>
    "$OUT/$1/run" "$ROM" "$FRAMES" > "$2" 2>"$OUT/$1.log"
}

cmp_hashes() {   # cmp_hashes <a> <b> <label>
    if cmp -s "$1" "$2"; then
        return 0
    fi
    local line
    line=$(diff "$1" "$2" | grep -m1 '^<' | awk '{print $1}' | tr -d '<' )
    echo "    first divergence: frame $(diff "$1" "$2" | grep -m1 '^< ' | awk '{print $2}')"
    return 1
}

prove_e2e() {
    echo
    echo "=== BEHAVIOUR: the whole game, hook off vs hook on, $FRAMES frames"
    echo "    (hashing screen + audio + IWRAM + EWRAM + VRAM + palette + OAM + I/O + clock)"
    build e2e_off "-DM4A_HASH"                 || return 1
    build e2e_on  "-DGBA_M4A_HLE -DM4A_HASH"   || return 1

    run_hashes e2e_off "$OUT/off.txt" || return 1
    run_hashes e2e_on  "$OUT/on.txt"  || return 1
    grep -q "hooked at" "$OUT/e2e_on.log" || {
        echo "FAIL: nothing was hooked — this ROM proves nothing about the hook."
        grep "M4A:" "$OUT/e2e_on.log"
        return 1
    }
    grep "M4A:" "$OUT/e2e_on.log" | sed 's/^/    /'

    if ! cmp_hashes "$OUT/off.txt" "$OUT/on.txt"; then
        echo "FAIL: the game behaves differently with the hook on."
        return 1
    fi
    echo "    identical, all $FRAMES frames"

    echo "--- RED: the same comparison with the transliteration deliberately wrong"
    build e2e_red "-DGBA_M4A_HLE -DM4A_HASH -DM4A_SABOTAGE" || return 1
    run_hashes e2e_red "$OUT/red.txt" || return 1
    if cmp -s "$OUT/off.txt" "$OUT/red.txt"; then
        echo "FAIL: a sabotaged mixer produced an identical game. The hash proves nothing."
        return 1
    fi
    echo "--- RED confirmed: the sabotaged mixer is caught"
}

# ---------------------------------------------------------------- worth it?
prove_speed() {
    echo
    echo "=== SPEED: the same $FRAMES frames, interpreted vs hooked"
    build sp_off "-DM4A_COUNT_INSNS"                  || return 1
    build sp_on  "-DGBA_M4A_HLE -DM4A_COUNT_INSNS"    || return 1
    echo "--- interpreter only:"
    "$OUT/sp_off/run" "$ROM" "$FRAMES" 2>&1 >/dev/null | sed 's/^/    /'
    echo "--- with the mixer hooked:"
    "$OUT/sp_on/run"  "$ROM" "$FRAMES" 2>&1 >/dev/null | sed 's/^/    /'
    echo "    (host fps is not device fps — but the ratio is the work removed)"
}

case "$MODE" in
    blocks) prove_blocks || rc=1 ;;
    e2e)    prove_e2e    || rc=1 ;;
    speed)  prove_speed  || rc=1 ;;
    all)    prove_blocks || rc=1
            [ $rc -eq 0 ] && { prove_e2e   || rc=1; }
            [ $rc -eq 0 ] && { prove_speed || rc=1; } ;;
esac

echo
if [ $rc -eq 0 ]; then
    echo "PASS: the native mixer computes the same thing, and the game cannot tell."
else
    echo "FAIL: see above."
fi
exit $rc
