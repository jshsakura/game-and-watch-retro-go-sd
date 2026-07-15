#!/bin/bash
# Apply Sega CD / gwenesis patches to the submodules. Run after
# `git submodule update --init`. Build with CHECK_DIRTY_SUBMODULE=0 (Docker does).
set -euo pipefail
cd "$(dirname "$0")/.."
git -C external/gwenesis apply --check patches/../patches/gwenesis-ym2612-silent-channel-skip.patch 2>/dev/null \
  && git -C external/gwenesis apply patches/../patches/gwenesis-ym2612-silent-channel-skip.patch \
  && echo "applied: gwenesis ym2612 silent-channel skip" \
  || echo "already applied or conflicts: gwenesis ym2612"
