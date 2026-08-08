#!/usr/bin/env python3
"""Turn a list of sampled PCs into a SNES-core profile.

Every emulator core is an overlay linked at the same RAM address, so gdb's
"info symbol" answers with whichever overlay it finds first -- on this ELF it
happily calls the SNES interpreter a Cyrillic font. Resolve against the SNES
core's own sections only (.overlay_snes*, .itcm_snes_interp*), and say plainly
when an address belongs to neither.

  resolve_snes.py <elf> <pcs.txt>
"""
import bisect
import subprocess
import sys
from collections import Counter

SNES_SECTIONS = (".overlay_snes", ".itcm_snes_interp")


def snes_symbols(elf):
    """(addr, name, section) for every symbol the SNES core owns, sorted."""
    out = subprocess.run(["arm-none-eabi-objdump", "-t", elf],
                         capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        f = line.split()
        if len(f) < 5:
            continue
        sec = next((x for x in f if x.startswith(SNES_SECTIONS)), None)
        if not sec:
            continue
        name = f[-1]
        if name.startswith("."):          # the section symbol itself
            continue
        try:
            syms.append((int(f[0], 16), name, sec))
        except ValueError:
            continue
    syms.sort()
    return syms


def resident_symbols(elf):
    """Symbols in internal flash. Only one program lives there, so unlike the
    overlays these names are unambiguous -- and time spent here is time the
    emulator is NOT running: the launcher, the shared frame loop, the ISRs."""
    out = subprocess.run(["arm-none-eabi-objdump", "-t", elf],
                         capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        f = line.split()
        if len(f) < 5 or not f[0].startswith("08"):
            continue
        name = f[-1]
        if name.startswith("."):
            continue
        try:
            addr = int(f[0], 16)
        except ValueError:
            continue
        if 0x08000000 <= addr < 0x08200000:
            syms.append((addr, name, "flash"))
    syms.sort()
    return syms


def region(addr):
    """What the address is, when no SNES symbol claims it."""
    if 0x08000000 <= addr < 0x08200000:
        return "internal flash (launcher/resident)"
    if 0x90000000 <= addr < 0x92000000:
        return "external flash XIP"
    if 0x24000000 <= addr < 0x24080000:
        return "RAM_EMU (another overlay's symbols)"
    if addr < 0x00010000:
        return "ITCM"
    if 0x20000000 <= addr < 0x20020000:
        return "DTCM"
    if 0x30000000 <= addr < 0x30048000:
        return "AHB SRAM"
    return "?"


def main():
    elf, pcs_file = sys.argv[1], sys.argv[2]
    syms = sorted(snes_symbols(elf) + resident_symbols(elf))
    addrs = [s[0] for s in syms]

    pcs = [int(x, 16) for x in open(pcs_file).read().split() if x.startswith("0x")]
    if not pcs:
        sys.exit("no PCs in " + pcs_file)

    named = Counter()
    for pc in pcs:
        i = bisect.bisect_right(addrs, pc) - 1
        # Accept the symbol only if the PC is plausibly inside it. Without sizes
        # from objdump -t, bound it by the next symbol -- and refuse a match
        # more than 8 KB in, which is how a wrong section quietly wins.
        if i >= 0 and (i + 1 >= len(addrs) or pc < addrs[i + 1]) and pc - addrs[i] < 0x2000:
            named[f"{syms[i][1]}  [{syms[i][2]}]"] += 1
        else:
            named[f"({region(pc)})"] += 1

    total = len(pcs)
    print(f"{total} samples, {len(syms)} symbols (SNES overlay + resident flash)\n")
    for name, n in named.most_common(30):
        print(f"{n:6d}  {100.0*n/total:5.1f}%  {name}")


if __name__ == "__main__":
    main()
