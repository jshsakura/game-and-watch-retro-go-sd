#!/usr/bin/env bash
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d /tmp/gnw-hw-harness.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

python3 "$here/test_gnw_hw.py"
python3 "$here/test_timing_oracle.py"
${CC:-gcc} -O2 -Wall -Wextra -Werror -std=c11 \
    -I"$here" "$here/alloc_model.c" "$here/test_alloc_model.c" \
    -o "$tmp/test_alloc_model"
"$tmp/test_alloc_model"
