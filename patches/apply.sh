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
# The picodrive patch is the one that matters for 32X speed, so it must never be
# skipped quietly: a silent skip once shipped a build with no patch in it.
PD_PATCH=../../patches/picodrive-32x-sh2-loop.patch
if git -C external/picodrive apply --check "$PD_PATCH" 2>/dev/null; then
  git -C external/picodrive apply "$PD_PATCH" && echo "applied: picodrive-32x-sh2-loop"
elif git -C external/picodrive apply --check --reverse "$PD_PATCH" 2>/dev/null; then
  echo "already applied: picodrive-32x-sh2-loop"
else
  echo "ERROR: patches/picodrive-32x-sh2-loop.patch neither applies nor is already applied to external/picodrive ($(git -C external/picodrive rev-parse --short HEAD))." >&2
  echo "       The submodule pin moved or the tree is dirty. Rebase the patch before building." >&2
  exit 1
fi
