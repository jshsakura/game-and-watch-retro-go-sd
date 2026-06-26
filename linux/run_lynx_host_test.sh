#!/bin/bash
#
# Atari Lynx (Handy) HOST core test — runs in CI alongside the firmware build so
# the Lynx core (external/handy-go, shared verbatim with the device overlay) is
# exercised under AddressSanitizer before anything is flashed.
#
# It builds the headless harness (linux/lynx/main.cxx + the handy core) and runs
# it over SYNTHETIC cartridge fixtures (no copyrighted ROMs shipped):
#   * test.lyx       — a minimal valid LNX cart (CCart RAM+pad path)
#   * test_bs93.lyx  — a minimal BS93 homebrew image. This is the regression
#                      guard for the big-endian BS93 header fix: with the old
#                      little-endian parse the load address underflowed and
#                      CRam::Reset OOB-wrote the 64K RAM (ASan SEGV); the fix
#                      makes it load + run cleanly.
#
# Any ASan/UBSan error or non-zero exit FAILS the build. Run from linux/.
set -euo pipefail
cd "$(dirname "$0")"

CORE=../external/handy-go
OUT=build_lynx
mkdir -p "$OUT"

# --- fixtures ---------------------------------------------------------------
bash ./update_lynx_rom.sh >/dev/null   # writes lynx/test.lyx (valid LNX)

python3 - lynx/test_bs93.lyx <<'PY'
import struct, sys
# Minimal BS93 homebrew (big-endian header, see ram.h CRam):
#   [0-1] = 80 08  -> 65C02 "BRA +8": skip the 10-byte header to the code
#   [2-3] = 02 00  -> load address 0x0200 (BIG-ENDIAN)
#   [4-5] = 00 20  -> size 0x0020 (BIG-ENDIAN)
#   [6-9] = "BS93" -> magic (CSystem classifies HANDY_FILETYPE_HOMEBREW)
#   [10+] = 4C 00 02 (JMP $0200) -> tight idle loop, no memory writes
data = bytes([0x80, 0x08, 0x02, 0x00, 0x00, 0x20]) + b"BS93" + bytes([0x4C, 0x00, 0x02])
data += b"\x00" * (32 - len(data))     # pad to the declared size
open(sys.argv[1], "wb").write(data)
print("wrote", sys.argv[1], len(data), "bytes")
PY

# --- build (ASan + UBSan) ---------------------------------------------------
g++ -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DIS_LITTLE_ENDIAN -DTARGET_GNW -DLINUX_EMU -DHAVE_STDINT_H \
  -I. -I./lynx -I"$CORE" \
  lynx/main.cxx "$CORE"/system.cpp "$CORE"/cart.cpp "$CORE"/mikie.cpp \
  "$CORE"/susie.cpp "$CORE"/eeprom.cpp "$CORE"/lynxdec.cpp \
  -lm -o "$OUT"/lynx_hosttest

# --- run --------------------------------------------------------------------
export ASAN_OPTIONS=abort_on_error=1:halt_on_error=1:detect_leaks=0
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
# fixture -> frame count. The BS93 regression (the OOB-write SEGV) happens at
# CONSTRUCTION (CSystem ctor -> CRam::Reset), before any frame, so 0 frames is a
# complete guard and avoids the dummy cart's 65C02 running garbage (which floods
# "illegal opcode" logs). The valid LNX cart renders a few frames too.
# stdout -> /dev/null (noise); ASan/UBSan diagnostics go to stderr and remain.
rc=0
run_fx() {
    local fx="$1" frames="$2"
    echo "=== running harness on $fx ($frames frames) ==="
    if "$OUT"/lynx_hosttest "$fx" "$frames" >/dev/null; then
        echo "PASS: $fx"
    else
        echo "FAIL: $fx (exit $?)"; rc=1
    fi
}
# Both synthetic carts contain no real 65C02 program, so running frames just
# floods "illegal opcode" logs. 0 frames exercises the full load path — CSystem
# ctor, CCart (LNX) / CRam (BS93) construction (the OOB-write crash site),
# file-type classification and ContextSave/Load — which is the regression guard
# we need. The render-path fix (mLynxAddr mask) is verified separately against
# the real-ROM ASan sweep, which can't be shipped to CI.
run_fx lynx/test.lyx      0
run_fx lynx/test_bs93.lyx 0
[ $rc -eq 0 ] && echo "===== Lynx host core test: ALL PASS =====" \
              || echo "===== Lynx host core test: FAILURES ====="
exit $rc
