#!/usr/bin/env bash
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
tmp=$(mktemp -d /tmp/gnw-super-p0.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
cc=${CC:-gcc}

alignment() {
    "$cc" -O1 -g -fsanitize=alignment -fno-sanitize-recover=alignment \
        "$here/fixtures/red/unaligned_u64.c" -o "$tmp/unaligned"
    if UBSAN_OPTIONS=halt_on_error=1 "$tmp/unaligned" >"$tmp/align.log" 2>&1; then
        echo "FAIL RED unaligned uint64_t access escaped the alignment sanitizer"
        return 1
    fi
    if ! grep -q "requires 8 byte alignment" "$tmp/align.log"; then
        echo "FAIL RED alignment case failed for the wrong reason"
        cat "$tmp/align.log"
        return 1
    fi
    echo "PASS RED: Cortex-M7 LDRD/STRD alignment class reproduced"
}

implicit_decl() {
    if "$cc" -std=c11 -Werror=implicit-function-declaration \
        "$here/fixtures/red/implicit_pointer.c" -o "$tmp/implicit" >"$tmp/implicit.log" 2>&1; then
        echo "FAIL RED implicit allocator declaration compiled"
        return 1
    fi
    grep -q "implicit declaration" "$tmp/implicit.log" || {
        echo "FAIL RED implicit declaration failed for the wrong reason"
        cat "$tmp/implicit.log"
        return 1
    }
    echo "PASS RED: implicit pointer-return declaration rejected"
}

poison() {
    "$cc" -O2 -Wall -Wextra -Werror -std=c11 -I"$here" \
        "$here/alloc_model.c" "$here/test_alloc_model.c" -o "$tmp/alloc"
    "$tmp/alloc"
}

parity() {
    cd "$repo"
    if [[ ! -f external/sm/src/snes/ppu.c ]]; then
        echo "SKIP: external/sm is not checked out; actual device source parity unavailable"
        return 77
    fi
    local run_out="$tmp/device_run"
    local run_log="$tmp/device_run.log"
    if ! OUT="$run_out" tools/sm_harness/device_run.sh >"$run_log" 2>&1; then
        cat "$run_log"
        if grep -q "undefined reference to.*apu_run" "$run_log" \
           && ! grep "undefined reference to" "$run_log" | grep -vq "apu_run" \
           && grep -q '^void apu_run' Core/Src/porting/sm/main_sm.c; then
            "$cc" -c -O1 -g -fsanitize=alignment "$here/sm_firmware_seams.c" \
                -o "$run_out/gnw_firmware_seams.o"
            "$cc" -fsanitize=alignment -o "$run_out/sm_device" "$run_out"/*.o -lm
            echo "PASS actual device source set compiled; explicit main_sm apu_run seam linked"
        else
            return 1
        fi
    else
        cat "$run_log"
    fi

    local parity_out="$tmp/sm_parity"
    if ! tools/sm_harness/device_parity.sh "$parity_out"; then
        if grep -q "undefined reference to.*apu_run" "$parity_out/err.txt" \
           && ! grep "undefined reference to" "$parity_out/err.txt" | grep -vq "apu_run" \
           && grep -q '^void apu_run' Core/Src/porting/sm/main_sm.c; then
            "$cc" -c -Os "$here/sm_firmware_seams.c" -o "$parity_out/gnw_firmware_seams.o"
            "$cc" -o "$parity_out/sm_parity" "$parity_out"/*.o -lm
            echo "PASS symbol parity complete with explicit firmware-owned apu_run seam"
        else
            return 1
        fi
    fi
}

self_identical() {
    "$cc" -O2 "$here/fixtures/green/deterministic.c" -o "$tmp/deterministic"
    "$cc" -O2 "$here/fixtures/red/nondeterministic.c" -o "$tmp/nondeterministic"
    python3 "$here/repeatability.py" --runs 3 -- "$tmp/deterministic"
    if python3 "$here/repeatability.py" --runs 3 -- "$tmp/nondeterministic"; then
        echo "FAIL RED nondeterministic comparator fixture passed"
        return 1
    fi
    echo "PASS RED: self-disagreement is rejected"
}

case "${1:-all}" in
    alignment) alignment ;;
    implicit-decl) implicit_decl ;;
    poison) poison ;;
    parity) parity ;;
    self-identical) self_identical ;;
    all) alignment; implicit_decl; poison; parity; self_identical ;;
    *) echo "usage: $0 [alignment|implicit-decl|poison|parity|self-identical|all]" >&2; exit 2 ;;
esac
