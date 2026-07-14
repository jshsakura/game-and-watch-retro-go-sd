#!/usr/bin/env bash
# Runs gpSP's cart-load path on the host, the way the device runs it.
#
# The point of building it here rather than in QEMU: page zero is unmapped on a
# hosted OS, and it is NOT unmapped on an mps2-an500. gpSP's save-type detection
# used to read the cart through gamepak_buffers[0], which is NULL on an XIP build
# (ROM_BUFFER_SIZE=0 — the cart stays in flash and is never buffered). On QEMU
# that read a megabyte from address 0 and shrugged; on the device the first 64 KB
# of address 0 is ITCM and the next byte is nothing, so Pokemon Ruby died in a bus
# fault inside memcmp. Here it is a SIGSEGV, which is the honest answer.
#
# Same source set as the firmware (Makefile's GBA_C_SOURCES / GBA_CXX_SOURCES) and
# the same defines, including the real Core/Src/porting/gba/gba_frontend.c — a
# harness that links a different program than the device proves nothing.
#
#   ./run.sh          build and run  (exit 0 = the cart was read through the XIP pointer)
#   ./run.sh --red    build the pre-fix gba_memory.c out of git history and show it crash
set -u
cd "$(dirname "$0")/../.."

P=external/gpsp
OUT=/tmp/gba_harness
CC=${CC:-gcc}
CXX=${CXX:-g++}

if [ ! -f "$P/gba_memory.c" ]; then
    echo "SKIP: external/gpsp not checked out — run git submodule update --init"
    exit 0
fi

CF="-O1 -g -w"
CF="$CF -DROM_BUFFER_SIZE=0 -DOBJ_PER_LINE_MAX=32 -DBUFFER_SIZE=4096 -DGBA_SOUND_FREQUENCY=48000"
CF="$CF -I$P -I$P/libretro/libretro-common/include -Itools/gba_harness"
# The device traps what the host would otherwise wave through. Ask for the same.
CF="$CF -fsanitize=address -fno-omit-frame-pointer -Werror=implicit-function-declaration"

RED=0
[ "${1:-}" = "--red" ] && RED=1
# The commit that read the cart through gamepak_buffers[0]; its parent is where the
# fix lands. Override to point --red at a different revision.
GBA_PREFIX_REV="${GBA_PREFIX_REV:-fe18d2f}"

rm -rf "$OUT" && mkdir -p "$OUT"

MEM_SRC="$P/gba_memory.c"
if [ "$RED" = "1" ]; then
    # The file as it was before the fix, out of the submodule's own history. A test
    # that has never failed proves nothing — and it has to fail against the real
    # thing, not against a hand-broken copy of it.
    if ! ( cd "$P" && git show "$GBA_PREFIX_REV:gba_memory.c" ) > "$OUT/gba_memory_prefix.c" 2>/dev/null; then
        echo "SKIP: --red needs the submodule's git history (shallow clone?)"
        exit 0
    fi
    if ! grep -q "detect_backup_subcircuit(gamepak_buffers\[0\]" "$OUT/gba_memory_prefix.c"; then
        echo "SKIP: the submodule's HEAD already has the fix — nothing to go red against"
        echo "      (check out the commit before it to reproduce)"
        exit 0
    fi
    MEM_SRC="$OUT/gba_memory_prefix.c"
    echo "--- RED: building gba_memory.c from before the fix"
fi

$CC $CF -c "$MEM_SRC" -o "$OUT/gba_memory.o" || exit 1
for f in sound main savestate input cheats serial serial_proto gbp rfu; do
    $CC $CF -c "$P/$f.c" -o "$OUT/$f.o" || exit 1
done
for f in cpu video; do
    $CXX $CF -fno-exceptions -fno-rtti -c "$P/$f.cc" -o "$OUT/$f.o" || exit 1
done
# The device's own front-end, not a copy of it.
$CC $CF -c Core/Src/porting/gba/gba_frontend.c -o "$OUT/gba_frontend.o" || exit 1
$CC $CF -c tools/gba_harness/host_stubs.c      -o "$OUT/host_stubs.o"   || exit 1
# NOT main.o — gpSP has a main.c of its own, and it would be overwritten.
$CC $CF -c tools/gba_harness/load_gamepak_xip.c -o "$OUT/zz_harness.o"  || exit 1

$CXX -fsanitize=address "$OUT"/*.o -o "$OUT/t" -lm || exit 1

"$OUT/t"
rc=$?

if [ "$RED" = "1" ]; then
    if [ $rc -eq 0 ]; then
        echo "FAIL: the pre-fix code did NOT crash — this test no longer reproduces the bug"
        exit 1
    fi
    echo "--- RED confirmed: the pre-fix code dies reading the cart through NULL (rc=$rc)"
    exit 0
fi

[ $rc -eq 0 ] && echo "PASS: tools/gba_harness" || echo "FAIL: tools/gba_harness (rc=$rc)"
exit $rc
