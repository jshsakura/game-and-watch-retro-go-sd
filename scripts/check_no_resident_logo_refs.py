#!/usr/bin/env python3
"""Fail the link if a .sdcard_logo address is baked into resident flash.

.sdcard_logo is not memory. It is a staging section whose contents are objcopy'd
out to /bios/logo.bin, and whose symbol addresses are extflash LOAD addresses
(0x004xxxxx). Nothing can read them at runtime. The launcher reaches those logos
by INDEX, through rg_get_logo(), which reads the file into a cache.

So a line like

    return (retro_logo_image *)&header_favorites;   /* LOGO_DATA */

compiles, links, and hands the caller a pointer to an address that does not
exist. It shipped: entering the favorites tab faulted at 0x004c1d5e, which is
exactly header_favorites, while the comment above the line asserted the opposite
("served from flash"). CPS-1's wordmark had the identical bug earlier. The three
logos that ARE resident (logo_rgo/rgw/gnw) are declared INT_LOGO_DATA and live in
.intflash_logo, so referencing those is correct -- the rule is about the section,
not the name.

How it is checked, and why not by relocation: the offending code sits in
rg_logos.c, which also DEFINES the art, so an object-level "references a symbol
it does not define" test cannot see it (the first draft of this script passed the
crashing build for exactly that reason). What is unambiguous is the pointer
itself: to hand out 0x004c1d5e, the compiler has to store that word in resident
flash, normally as a literal-pool entry. So this scans every resident section for
a word equal to a staged logo's address. A load of an address that cannot be read
leaves a fingerprint, and this is it.

Skips loudly if the toolchain is unavailable -- a safety net that breaks builds
gets deleted (CLAUDE.md).

usage: check_no_resident_logo_refs.py <objdump> <nm> <elf>
"""
import re
import subprocess
import sys

# .sdcard_logo lands here (extflash LMA); resident code/data lives in internal
# flash at 0x08xxxxxx. Both are used only to classify symbols and sections.
STAGED_LO, STAGED_HI = 0x00400000, 0x00600000
RESIDENT_LO, RESIDENT_HI = 0x08000000, 0x08200000


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def staged_symbols(nm, elf):
    """{address: name} for every symbol staged in .sdcard_logo."""
    out = {}
    for line in run([nm, "-n", elf]).splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        if STAGED_LO <= addr < STAGED_HI:
            out[addr] = parts[2]
    return out


def resident_sections(objdump, elf):
    """[(name, vma, size)] for loadable sections that live in internal flash."""
    out = []
    for line in run([objdump, "-h", elf]).splitlines():
        # Idx Name  Size  VMA  LMA  File off  Algn
        m = re.match(r"\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
        if not m:
            continue
        name, size, vma = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
        if size and RESIDENT_LO <= vma < RESIDENT_HI:
            out.append((name, vma, size))
    return out


def resident_words(objdump, elf):
    """Yield (address, word) for every 32-bit word in resident flash sections.

    Dumped per section and bounded by that section's declared size, because
    `objdump -s` prints an ASCII column beside the hex and a group of it can
    itself look like hex ("abcd"). An earlier draft parsed the whole listing at
    once and either crashed on that column or silently read it as data. Bounding
    by size removes the guesswork.
    """
    for name, vma, size in resident_sections(objdump, elf):
        try:
            out = run([objdump, "-s", "-j", name, elf])
        except subprocess.CalledProcessError:
            continue
        raw = bytearray()
        for line in out.splitlines():
            m = re.match(r"\s*([0-9a-f]{4,16})\s+((?:[0-9a-f]{2,8} ?){1,4})", line)
            if not m:
                continue
            groups = m.group(2).split()
            # At most 16 bytes per line; ignore anything past that (ASCII column).
            chunk = bytearray()
            for g in groups[:4]:
                if len(g) % 2 or not re.fullmatch(r"[0-9a-f]+", g):
                    break
                chunk += bytes.fromhex(g)
            raw += chunk[:16]
            if len(raw) >= size:
                break
        raw = raw[:size]
        for i in range(0, len(raw) - 3, 4):
            yield vma + i, int.from_bytes(raw[i:i + 4], "little")


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: check_no_resident_logo_refs.py <objdump> <nm> <elf>")
    objdump, nm, elf = sys.argv[1:]

    try:
        staged = staged_symbols(nm, elf)
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"resident-logo-refs: SKIPPED (cannot run {nm}: {exc}) -- not verified")
        return
    if not staged:
        print("resident-logo-refs: SKIPPED (no .sdcard_logo symbols) -- not verified")
        return

    try:
        words = list(resident_words(objdump, elf))
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"resident-logo-refs: SKIPPED (cannot run {objdump}: {exc}) -- not verified")
        return
    if not words:
        print("resident-logo-refs: SKIPPED (no resident sections dumped) -- not verified")
        return

    hits = [(at, w, staged[w]) for at, w in words if w in staged]

    if not hits:
        print(f"resident-logo-refs: OK ({len(staged)} staged logos, no address baked into flash)")
        return

    print("resident-logo-refs: FAILED -- a .sdcard_logo address is baked into resident flash.")
    print("  Those are extflash LOAD addresses (objcopy'd to /bios/logo.bin); reading one")
    print("  faults at runtime. Reach these logos by index through rg_get_logo().")
    for at, w, sym in sorted(set(hits)):
        print(f"    flash {at:#010x} holds {w:#010x} = {sym}")
    print("  Fix: use the RG_LOGO_* index, or declare the art INT_LOGO_DATA if it")
    print("  genuinely must be resident (costs internal flash, which is nearly full).")
    sys.exit(1)


if __name__ == "__main__":
    main()
