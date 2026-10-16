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
# `patch` rather than `git apply`: inside the docker build a git worktree's .git
# file points outside the mounted tree, so git cannot see the repo there.
PD_DIR=external/picodrive
# Order matters: the md-defer patch is written against the sh2-loop result.
for pd in picodrive-32x-sh2-loop picodrive-32x-md-defer picodrive-32x-md-defer-palprio picodrive-32x-md-defer-satskip picodrive-32x-md-defer-cramsync picodrive-segacd-cdda-prefetch; do
  PD_PATCH=$PWD/patches/$pd.patch
  if patch -d "$PD_DIR" -p1 --dry-run -s < "$PD_PATCH" >/dev/null 2>&1; then
    patch -d "$PD_DIR" -p1 -s < "$PD_PATCH" && echo "applied: $pd"
  elif patch -d "$PD_DIR" -p1 -R --dry-run -s < "$PD_PATCH" >/dev/null 2>&1; then
    echo "already applied: $pd"
  else
    echo "ERROR: patches/$pd.patch neither applies nor is already applied to $PD_DIR." >&2
    echo "       The submodule pin moved or the tree is dirty. Rebase the patch before building." >&2
    exit 1
  fi
done
