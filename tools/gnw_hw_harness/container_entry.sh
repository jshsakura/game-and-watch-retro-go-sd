#!/usr/bin/env bash
set -euo pipefail

gcc_version=$(arm-none-eabi-gcc -dumpfullversion)
qemu_version=$(qemu-system-arm --version | head -1)

case "$gcc_version" in
    15.2*) ;;
    *) echo "ERROR: expected ARM GCC 15.2.rel1 family, got $gcc_version" >&2; exit 2 ;;
esac
case "$qemu_version" in
    *"8.2.2"*) ;;
    *) echo "ERROR: expected QEMU 8.2.2, got $qemu_version" >&2; exit 2 ;;
esac

echo "ARM GCC: $gcc_version"
echo "QEMU: $qemu_version"

if (($#)); then
    exec "$@"
fi
exec tools/gnw_hw_harness/run_tests.sh
