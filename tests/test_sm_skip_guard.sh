#!/usr/bin/env bash
# Pins that tests/run.sh SKIPS the sm device-parity harness — instead of
# dying — when external/sm isn't checked out. That is exactly CI's
# host-tests job: it checks out without submodules (see
# .github/workflows/package.yml), so external/sm is an empty directory
# there, and the pre-fix run.sh called tools/sm_harness/device_parity.sh
# unconditionally and died on a missing sm_rtl.c.
#
# This extracts the guarded tail of the REAL, current tests/run.sh (from the
# "=== sm: ..." marker to EOF) rather than re-implementing the guard, so the
# test tracks whatever that file actually contains.
set -u
cd "$(dirname "$0")/.."
RUN_SH="${RUN_SH:-tests/run.sh}"
T=$(mktemp -d /tmp/sm_skip_test.XXXXXX)
rc=0

sed -n '/^echo "=== sm: device source set is symbol-complete ==="/,$p' "$RUN_SH" > "$T/fragment.sh"
if [ ! -s "$T/fragment.sh" ]; then
    echo "FAIL could not find the sm guard block in $RUN_SH (marker line missing/renamed)"
    rm -rf "$T"
    exit 1
fi

# Hide external/sm the way CI's checkout-without-submodules leaves it: the
# directory exists (git made the placeholder) but is empty, no src/sm_rtl.c.
HID_REAL_SM=0
if [ -f external/sm/src/sm_rtl.c ]; then
    HID_REAL_SM=1
    mv external/sm "$T/sm_real"
    mkdir -p external/sm
fi
restore_sm() {
    if [ "$HID_REAL_SM" -eq 1 ] && [ -d "$T/sm_real" ]; then
        rmdir external/sm 2>/dev/null || rm -rf external/sm
        mv "$T/sm_real" external/sm
        HID_REAL_SM=0
    fi
}
trap 'restore_sm; rm -rf "$T"' EXIT

echo "=== tests/run.sh: skips sm parity (not fails) when external/sm is absent ==="
bash "$T/fragment.sh" >"$T/out.txt" 2>&1
frag_rc=$?
if [ "$frag_rc" -eq 0 ] && grep -q "SKIP.*external/sm" "$T/out.txt"; then
    echo "OK  skipped and said so, exit 0"
else
    echo "FAIL expected exit 0 with a SKIP line naming external/sm, got exit=$frag_rc:"
    cat "$T/out.txt"
    rc=1
fi

# Sanity check the other side, when it's available locally: prove this isn't
# an "always skip" guard that would silently stop testing the real harness.
# Only runs where external/sm was actually checked out (never in host-tests
# CI, which never has it — nothing to restore there, nothing extra to run).
if [ "$HID_REAL_SM" -eq 1 ]; then
    restore_sm
    echo "=== tests/run.sh: still actually runs sm parity when external/sm IS present ==="
    bash "$T/fragment.sh" >"$T/out2.txt" 2>&1
    frag2_rc=$?
    if [ "$frag2_rc" -eq 0 ] && grep -q "device's source set is symbol-complete" "$T/out2.txt" \
        && ! grep -q "SKIP" "$T/out2.txt"; then
        echo "OK  ran the real device-parity harness, no SKIP printed"
    else
        echo "FAIL expected the real harness to run and pass, got exit=$frag2_rc:"
        cat "$T/out2.txt"
        rc=1
    fi
fi

exit $rc
