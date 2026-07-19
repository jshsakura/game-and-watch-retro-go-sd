#!/usr/bin/env bash
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
cd "$repo"

case "${1:-}" in
    alias-red)
        exec bash tests/test_check_core_symbol_aliases.sh
        ;;
    alias-live)
        elf=${GNW_ELF:-build/gw_retro_go.elf}
        if [[ ! -f "$elf" ]]; then
            echo "SKIP: linked ELF absent; live cross-overlay objdump check needs a canonical build"
            exit 77
        fi
        NM=${NM:-arm-none-eabi-nm} python3 scripts/check_core_symbol_aliases.py build "$elf"
        ;;
    xip-live)
        out=$(mktemp /tmp/gnw-xip.XXXXXX)
        trap 'rm -f "$out"' EXIT
        if ! bash tests/test_gba_xip_contract.sh | tee "$out"; then
            exit 1
        fi
        if grep -q '^SKIP:' "$out"; then exit 77; fi
        ;;
    wiring)
        bash tests/test_idle_timeout_wired.sh
        bash tests/test_boot_rescue_wired.sh
        out=$(mktemp /tmp/gnw-wired.XXXXXX)
        trap 'rm -f "$out"' EXIT
        if ! bash tests/test_gba_m4a_wired.sh | tee "$out"; then exit 1; fi
        if grep -q '^SKIP:' "$out"; then
            echo "SKIP: host wiring pins passed; linked GBA wiring inspection needs an ELF"
            exit 77
        fi
        ;;
    savestate)
        python3 "$here/source_audit.py" savestate
        tmp=$(mktemp -d /tmp/gnw-state.XXXXXX)
        trap 'rm -rf "$tmp"' EXIT
        ${CC:-gcc} -O2 -Wall -Wextra -std=c11 -ICore/Inc/porting/sm \
            tests/test_sm_state_header.c -o "$tmp/test_sm_state_header"
        "$tmp/test_sm_state_header"
        ;;
    alignment-live)
        if [[ -z ${GNW_SM_ROM:-} || ! -f ${GNW_SM_ROM:-} ]]; then
            echo "SKIP: set GNW_SM_ROM to run the actual SM core under alignment UBSan"
            exit 77
        fi
        exec tools/sm_harness/device_run.sh "$GNW_SM_ROM" "${GNW_SM_FRAMES:-300}"
        ;;
    repeat-live)
        if [[ -z ${GNW_REPEAT_COMMAND:-} ]]; then
            echo "SKIP: set GNW_REPEAT_COMMAND to the deterministic rig command"
            exit 77
        fi
        # Explicit operator input, split as a command line without eval.
        read -r -a command <<<"$GNW_REPEAT_COMMAND"
        exec python3 "$here/repeatability.py" --runs "${GNW_REPEAT_RUNS:-3}" -- "${command[@]}"
        ;;
    *)
        echo "usage: $0 alias-red|alias-live|xip-live|wiring|savestate|alignment-live|repeat-live" >&2
        exit 2
        ;;
esac
