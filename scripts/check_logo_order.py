#!/usr/bin/env python3
"""Prove the RG_LOGO_HEADER_* enum matches the order the linker actually emitted.

The enum is an INDEX into /bios/logo.bin, and the index is decided by the order
the `header_*` objects land in .sdcard_logo -- which is the LINK order, not the
declaration order. GCC reorders top-level definitions (-ftoplevel-reorder is on
from -O1), so the two are not the same thing and nothing warns when they part.

This has now gone wrong twice, both times silently:

  * cfa39466: header_cps1 is declared after header_segacd and was emitted
    before it, so Sega CD's wordmark appeared on the CPS-1 tab on the device.
  * 0722: taking upstream's header_lynx (and, separately, moving 178 lines of
    colour icons into rg_logos_fork.c) each shuffled the emitted order again.

Both compile, both link, and both look correct in every log. The only way to
see them is to compare the enum against nm output, which is what this does --
at link time, so it cannot be forgotten.

usage: check_logo_order.py <elf> <bitmaps.h>
"""
import re
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: check_logo_order.py <elf> <bitmaps.h>", file=sys.stderr)
        return 2
    elf, header = sys.argv[1], sys.argv[2]
    nm = __import__("os").environ.get("NM", "arm-none-eabi-nm")

    try:
        out = subprocess.run([nm, "-n", elf], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        # Skip loudly rather than fail: a safety net that breaks the build over
        # a missing tool teaches people to delete the safety net.
        print(f"check_logo_order: SKIP ({nm}: {exc}) -- logo order NOT verified",
              file=sys.stderr)
        return 0

    linked = [ln.split()[2] for ln in out.splitlines()
              if re.search(r"\s[Rr]\sheader_\w+$", ln)]
    if not linked:
        print("check_logo_order: SKIP (no header_* symbols in the ELF)", file=sys.stderr)
        return 0

    src = open(header, encoding="utf-8", errors="replace").read()
    seen, enum = set(), []
    for name in re.findall(r"RG_LOGO_HEADER_[A-Z0-9_]+", src):
        sym = "header_" + name[len("RG_LOGO_HEADER_"):].lower()
        if sym not in seen:
            seen.add(sym)
            enum.append(sym)

    bad = [(i, a, b) for i, (a, b) in enumerate(zip(enum, linked)) if a != b]
    missing = set(enum) ^ set(linked)

    if not bad and len(enum) == len(linked):
        print(f"check_logo_order: OK ({len(linked)} headers, enum == link order)",
              file=sys.stderr)
        return 0

    print("check_logo_order: FAIL -- the logo enum does not match the link order.",
          file=sys.stderr)
    print("  Every tab whose index moved will draw a DIFFERENT system's wordmark.",
          file=sys.stderr)
    for i, a, b in bad[:12]:
        print(f"    index {i}: enum says {a}, linker emitted {b}", file=sys.stderr)
    if missing:
        print(f"    only on one side: {sorted(missing)}", file=sys.stderr)
    print("  Fix bitmaps.h to match this order:", file=sys.stderr)
    for i, sym in enumerate(linked):
        print(f"    {i:3d}  RG_LOGO_HEADER_{sym[len('header_'):].upper()}",
              file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
