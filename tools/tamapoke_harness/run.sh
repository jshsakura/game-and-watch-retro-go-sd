#!/usr/bin/env bash
# Build and run the TamaPoke layout harness on the host.
#
# The source list is read out of the Makefile rather than copied here. A second
# list is a second program, and a harness that compiles something other than the
# firmware proves nothing about the firmware -- which is exactly how three
# Super Metroid releases shipped unbootable behind a green harness.
#
# The host is also made to enforce the device's CPU rules where it cheaply can:
# unaligned 64-bit accesses trap on a Cortex-M7 and are silent on x86, and an
# implicit declaration is a truncated pointer on a 32-bit target and a wild one
# on a 64-bit host.
#
#   ./run.sh [out_dir]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT_DIR="${1:-$REPO/build/tamapoke_screens}"
BUILD_DIR="$REPO/build/tamapoke_harness"

mkdir -p "$OUT_DIR" "$BUILD_DIR"

# Ask make itself for the list, so it can never drift from what links.
SOURCES="$(make -s -C "$REPO" -f Makefile -f /dev/stdin TAMAPOKE=1 __harness_sources <<'EOF'
__harness_sources:
	@echo $(TAMAPOKE_CXX_SOURCES)
EOF
)"

if [ -z "$SOURCES" ]; then
    echo "SKIP: TAMAPOKE_CXX_SOURCES is empty -- is the port wired into the Makefile?" >&2
    exit 0
fi

# The UI file is the one being iterated on; if it is not there yet the harness
# has nothing to render. Skip loudly rather than fail the tree.
if [ ! -f "$REPO/Core/Src/porting/tamapoke/tamapoke_ui.cpp" ]; then
    echo "SKIP: tamapoke_ui.cpp not written yet -- nothing to lay out" >&2
    exit 0
fi

CXXFLAGS=(
    -std=c++17 -O1 -g
    -DTAMAPOKE=1 -DTAMAPOKE_HARNESS=1
    -I"$HERE/include"                 # HAL stand-in, so the real gw_lcd.h builds
    -I"$REPO/Core/Inc"
    -I"$REPO/Core/Inc/retro-go"
    -I"$REPO/Core/Inc/porting/tamapoke"
    -I"$REPO/retro-go-stm32/components/odroid"
    -I"$REPO"
    # (C++ already rejects implicit declarations, so the C-only -Werror for
    # them is neither needed nor accepted here.)
    -Wall -Wno-unused-parameter -Wno-multichar
    -fsanitize=address,undefined,alignment
    -fno-omit-frame-pointer
)

echo "== compiling the firmware's own sources =="
OBJS=()
# main_tamapoke.cpp is the firmware entry: it pulls in common.h / gw_lcd.h /
# main.h / odroid_system.h and registers the app with the launcher. The
# harness has its own main (harness_main.cpp) and exercises only the UI
# surface, so the firmware entry brings nothing the harness needs.
for src in $SOURCES; do
    case "$src" in
        */main_tamapoke.cpp) echo "   (skip $src -- harness has its own entry)"; continue ;;
        */tamapoke_unicode.cpp) echo "   (skip $src -- host_stubs draws unicode)"; continue ;;
        */tamapoke_shim.cpp) echo "   (skip $src -- host_stubs provides millis/epoch)"; continue ;;
    esac
    obj="$BUILD_DIR/$(basename "${src%.*}").o"
    echo "   $src"
    g++ "${CXXFLAGS[@]}" -c "$REPO/$src" -o "$obj"
    OBJS+=("$obj")
done

for extra in host_stubs.cpp harness_main.cpp; do
    obj="$BUILD_DIR/${extra%.cpp}.o"
    g++ "${CXXFLAGS[@]}" -c "$HERE/$extra" -o "$obj"
    OBJS+=("$obj")
done

g++ "${CXXFLAGS[@]}" "${OBJS[@]}" -o "$BUILD_DIR/tamapoke_harness"

echo
echo "== rendering screens =="
cd "$REPO"
"$BUILD_DIR/tamapoke_harness" "$OUT_DIR"
