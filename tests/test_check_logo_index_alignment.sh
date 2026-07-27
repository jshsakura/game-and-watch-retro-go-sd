#!/usr/bin/env bash
# Pins scripts/check_logo_index_alignment.py — the link-time gate that keeps
# /bios/logo.bin's index space equal to the RG_LOGO_* enum in bitmaps.h.
#
# Why this gate exists, and why the test is worth its length: rg_get_logo() maps
# an enum value to a blob index as (value - INT_LOGO_COUNT), and the blob is a
# raw objcopy of .sdcard_logo — so the ONLY thing binding a name to a picture is
# that the enum's order equals the LINK ORDER of the LOGO_DATA structs. Nothing
# in the language enforces it, and GCC is free to reorder top-level definitions.
# That equality has broken twice, both times on an upstream merge:
#   0722 — three logos shifted; caught by a hand-run `nm -n` diff.
#   0727 — upstream added real pad_lynx art (linking at blob 36) and moved
#          header_lynx to blob 18, while this fork's enum still listed both far
#          down the tail. Every entry behind them served its neighbour's art;
#          HEADER_WSWAN and HEADER_PCECD handed 32-40px PADS to the 18px header
#          slot, and odroid_overlay_draw_logo() had no clipping, so drawing the
#          header bar wrote thousands of pixels past the framebuffer. Symptom:
#          the launcher died entering the grid home / favorites / settings, with
#          a BSOD holding a pointer that traced back to nothing.
#
# The fixtures are the real 0727 artefacts, not sketches:
#   fixtures/logo_link_order_0727.nm   — `nm -n` of that build's ELF, filtered
#                                        to the LOGO_DATA symbols. Real link
#                                        order, and unchanged by the fix (the
#                                        fix moved enum entries, not structs).
#   fixtures/bitmaps_0727_broken.h     — the header verbatim at ab336875, i.e.
#                                        the shipped bug.
# So case (b) is RED against the real thing, and (c) proves the repaired header
# in this tree is what makes it GREEN — against that same real link order.
#
# No cross toolchain is needed anywhere here: the checker only shells out to nm,
# so a 6-line Python stand-in replays the captured text. That matters because
# the host-tests CI job deliberately has no toolchain.
set -u
cd "$(dirname "$0")/.."
CHECKER="${CHECKER:-scripts/check_logo_index_alignment.py}"
HEADER="Core/Inc/retro-go/bitmaps.h"
FIX="tests/fixtures"
PY="$(command -v python3)"
T="$(mktemp -d /tmp/logoidx_test.XXXXXX)"
trap 'rm -rf "$T"' EXIT
rc=0

for f in "$FIX/logo_link_order_0727.nm" "$FIX/bitmaps_0727_broken.h" "$HEADER"; do
    if [ ! -f "$f" ]; then
        echo "FAIL missing fixture/input: $f"
        exit 1
    fi
done

# --- fake nm: replay the captured link order instead of reading a real ELF ---
cat > "$T/fakenm" <<'PYEOF'
#!/usr/bin/env python3
import sys, os
# The checker calls: <nm> -n <elf>. Our "elf" is a text file of nm output.
path = sys.argv[-1]
sys.stdout.write(open(path).read() if os.path.isfile(path) else "")
PYEOF
chmod +x "$T/fakenm"
FAKE_NM="$T/fakenm"
ELF="$FIX/logo_link_order_0727.nm"   # stands in for the ELF; fake nm echoes it

echo "=== check_logo_index_alignment: a missing nm must skip loudly, not fail the build ==="
# A safety net that breaks the build teaches people to ignore CI (CLAUDE.md).
# /nonexistent-nm cannot be executed; the checker must say so and exit 0.
"$PY" "$CHECKER" "$T/nonexistent-nm" "$ELF" "$HEADER" >"$T/a.out" 2>&1
a_rc=$?
if [ "$a_rc" -eq 0 ] && grep -q "SKIPPED" "$T/a.out"; then
    echo "OK  exits 0 and says SKIPPED when nm cannot be run"
else
    echo "FAIL missing-nm case: expected exit 0 with a SKIPPED line, got exit=$a_rc:"
    cat "$T/a.out"
    rc=1
fi

echo "=== check_logo_index_alignment: the shipped 0727 header must FAIL (this is the bug) ==="
"$PY" "$CHECKER" "$FAKE_NM" "$ELF" "$FIX/bitmaps_0727_broken.h" >"$T/b.out" 2>&1
b_rc=$?
# Demanding the specific names keeps this from passing for some unrelated reason:
# PAD_LYNX is the entry upstream inserted, and HEADER_WSWAN is the slot whose
# oversized pad actually corrupted memory.
if [ "$b_rc" -ne 0 ] \
   && grep -q "FAILED" "$T/b.out" \
   && grep -q "RG_LOGO_PAD_LYNX" "$T/b.out" \
   && grep -q "RG_LOGO_HEADER_WSWAN" "$T/b.out"; then
    echo "OK  exits non-zero and names PAD_LYNX and HEADER_WSWAN"
else
    echo "FAIL broken-header case: expected non-zero exit naming PAD_LYNX/HEADER_WSWAN, got exit=$b_rc:"
    cat "$T/b.out"
    rc=1
fi

echo "=== check_logo_index_alignment: this tree's header must PASS against that same link order ==="
"$PY" "$CHECKER" "$FAKE_NM" "$ELF" "$HEADER" >"$T/c.out" 2>&1
c_rc=$?
if [ "$c_rc" -eq 0 ] && grep -q "OK" "$T/c.out"; then
    echo "OK  the repaired enum aligns with the real 0727 link order"
else
    echo "FAIL current-header case: expected exit 0, got exit=$c_rc:"
    cat "$T/c.out"
    echo "     (If you just added a logo: an entry's enum value minus 3 must equal"
    echo "      its LOGO_DATA struct's position in \`nm -n\` order.)"
    rc=1
fi

echo "=== check_logo_index_alignment: a backed entry behind a colour-only one must FAIL ==="
# The other half of the 0727 fault, isolated: pad_lynx became blob-backed while
# still sitting in the colour-only tail. An unbacked slot in the middle shifts
# every backed entry behind it, so this ordering rule is not cosmetic.
cat > "$T/order.nm" <<'EOF'
004bea74 R header_gb
004beb2c R header_nes
004bec62 R pad_gb
EOF
cat > "$T/order.h" <<'EOF'
enum {
    RG_LOGO_EMPTY = -1,
    RG_LOGO_RGO = 0,
    RG_LOGO_RGW,
    RG_LOGO_GNW,
    RG_LOGO_HEADER_GB,
    RG_LOGO_HEADER_NES,
    RG_LOGO_PAD_PICO8,
    RG_LOGO_PAD_GB,
};
EOF
"$PY" "$CHECKER" "$FAKE_NM" "$T/order.nm" "$T/order.h" >"$T/d.out" 2>&1
d_rc=$?
if [ "$d_rc" -ne 0 ] && grep -q "AFTER colour-only" "$T/d.out"; then
    echo "OK  exits non-zero when a blob-backed entry sits after an unbacked one"
else
    echo "FAIL ordering case: expected non-zero exit mentioning 'AFTER colour-only', got exit=$d_rc:"
    cat "$T/d.out"
    rc=1
fi

echo "=== check_logo_index_alignment: a blob logo with no enum entry must FAIL ==="
# Art added to rg_logos.c without an enum entry is a silent +1 to every index
# behind it — the same failure, arriving from the other direction.
cat > "$T/extra.nm" <<'EOF'
004bea74 R header_gb
004beb2c R header_brandnew
004bec62 R header_nes
EOF
cat > "$T/extra.h" <<'EOF'
enum {
    RG_LOGO_EMPTY = -1,
    RG_LOGO_RGO = 0,
    RG_LOGO_RGW,
    RG_LOGO_GNW,
    RG_LOGO_HEADER_GB,
    RG_LOGO_HEADER_NES,
};
EOF
"$PY" "$CHECKER" "$FAKE_NM" "$T/extra.nm" "$T/extra.h" >"$T/e.out" 2>&1
e_rc=$?
if [ "$e_rc" -ne 0 ] && grep -q "header_brandnew" "$T/e.out"; then
    echo "OK  exits non-zero naming the unmapped blob logo"
else
    echo "FAIL unmapped-logo case: expected non-zero exit naming header_brandnew, got exit=$e_rc:"
    cat "$T/e.out"
    rc=1
fi

exit $rc
