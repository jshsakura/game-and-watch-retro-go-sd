#!/usr/bin/env bash
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
map="$repo/build/gw_retro_go.map"
elf="$repo/build/gw_retro_go.elf"
profile="${GNW_DEVICE_PROFILE:-}"
config="${GNW_BUILD_CONFIG:-}"
output="$repo/build/gnw_hw_contract.json"
golden=""
run_tests=0
run_all=0
require_device=0
verbose=0
declare -a proposals=()

usage() {
    echo "usage: $0 [--map FILE] [--elf FILE] [--profile FILE] [--config FILE] [--output FILE]" >&2
    echo "          [--golden FILE] [--proposal REGION:BYTES:LABEL] [--tests]" >&2
    echo "          [--all] [--require-device] [--verbose]" >&2
}

while (($#)); do
    case "$1" in
        --map) map=$2; shift 2 ;;
        --elf) elf=$2; shift 2 ;;
        --profile) profile=$2; shift 2 ;;
        --config) config=$2; shift 2 ;;
        --output) output=$2; shift 2 ;;
        --golden) golden=$2; shift 2 ;;
        --proposal) proposals+=("$2"); shift 2 ;;
        --tests) run_tests=1; shift ;;
        --all) run_all=1; shift ;;
        --require-device) require_device=1; shift ;;
        --verbose) verbose=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage; exit 2 ;;
    esac
done

if ((run_all)); then
    super_args=()
    ((require_device)) && super_args+=(--require-device)
    ((verbose)) && super_args+=(--verbose)
    exec python3 "$here/super_gate.py" "${super_args[@]}"
fi

if ((run_tests)); then
    "$here/run_tests.sh"
fi

if [[ ! -f "$map" ]]; then
    echo "ERROR: linker map not found: $map" >&2
    echo "Build the canonical SD firmware first, or pass --map." >&2
    exit 2
fi

mkdir -p "$(dirname "$output")"
args=(extract --map "$map" --output "$output" --report)
[[ -f "$elf" ]] && args+=(--elf "$elf")
[[ -n "$profile" ]] && args+=(--profile "$profile")
[[ -n "$config" ]] && args+=(--config "$config")
[[ -n "$golden" ]] && args+=(--golden "$golden")
python3 "$here/gnw_hw.py" "${args[@]}"

for proposal in "${proposals[@]}"; do
    IFS=: read -r region bytes label <<<"$proposal"
    if [[ -z "$region" || -z "$bytes" ]]; then
        echo "ERROR: bad proposal '$proposal' (expected REGION:BYTES:LABEL)" >&2
        exit 2
    fi
    python3 "$here/gnw_hw.py" check-allocation \
        --spec "$output" --region "$region" --bytes "$bytes" \
        --label "${label:-proposal}"
done

echo "contract: $output"
