#!/bin/bash
#
# ZX Spectrum (chips core) host harness — sound + input verification under ASan.
# Mirrors linux/lynx/run_lynx_host_test.sh. Run from the repo root:
#   bash linux/zx/run_zx_host_test.sh
#
# Proves on the host (so we don't guess on-device):
#   * the core CPU->beeper->audio-callback pipeline produces sound, and
#   * no joystick/button/key combination crashes the core (ASan/UBSan clean).
# The PAUSE-menu HardFault is a firmware NULL-repaint bug (fixed in main_zxs.c),
# not reproducible here because the firmware menu layer isn't linked.
set -euo pipefail
cd "$(dirname "$0")"

[ -f 48.rom ] || cp ../48.rom . 2>/dev/null || { echo "need a 48K Spectrum ROM at linux/zx/48.rom"; exit 1; }

gcc -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
    -I../../Core/Src/porting/zxs \
    -o zx_harness.elf zx_harness.c

ASAN_OPTIONS=detect_leaks=0 ./zx_harness.elf
