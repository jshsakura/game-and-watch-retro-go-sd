#!/bin/bash
# Apply the gwenesis lossless perf patches and the picodrive 32X SH-2 loop patch
# to their submodules.
# Run after `git submodule update --init`. Build with CHECK_DIRTY_SUBMODULE=0.
set -euo pipefail
cd "$(dirname "$0")/.."
for p in gwenesis-ym2612-silent-channel-skip gwenesis-vdp-scroll-hoist; do
  if git -C external/gwenesis apply --check "patches/$p.patch" 2>/dev/null; then
    git -C external/gwenesis apply "patches/$p.patch" && echo "applied: $p"
  else echo "skip (applied/conflict): $p"; fi
done
if git -C external/picodrive apply --check ../../patches/picodrive-32x-sh2-loop.patch 2>/dev/null; then
  git -C external/picodrive apply ../../patches/picodrive-32x-sh2-loop.patch && echo "applied: picodrive-32x-sh2-loop"
else echo "skip (applied/conflict): picodrive-32x-sh2-loop"; fi
