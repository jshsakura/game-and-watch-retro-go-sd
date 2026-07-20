#!/bin/bash
# Apply Sega CD / gwenesis lossless perf patches to the submodules.
# Run after `git submodule update --init`. Build with CHECK_DIRTY_SUBMODULE=0.
set -euo pipefail
cd "$(dirname "$0")/.."
for p in gwenesis-ym2612-silent-channel-skip gwenesis-vdp-scroll-hoist; do
  if git -C external/gwenesis apply --check "patches/$p.patch" 2>/dev/null; then
    git -C external/gwenesis apply "patches/$p.patch" && echo "applied: $p"
  else echo "skip (applied/conflict): $p"; fi
done
