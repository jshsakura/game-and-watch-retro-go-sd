#!/usr/bin/env python3
"""Fail the link if a boot-time constructor points into overlay RAM.

startup_stm32h7b0xx.s calls __libc_init_array() before main(), which walks the
RESIDENT .init_array in internal flash and calls every pointer in it. A pointer
that aims into RAM_EMU is a call into a region that, at that moment, holds
nothing: no core is loaded yet. The device faults before the LCD is initialised,
so there is no BSOD, no boot-rescue screen and no clue of any kind -- just a dead
black screen. The patched OFW still boots, which makes it look like anything but
a firmware fault.

This is not hypothetical. It shipped twice:
  * C64/Frodo, earlier: global constructors in the overlay leaked into the
    resident array and bricked boot.
  * TamaPoke, releases testbed-full-20260727-1501 and -1610: exactly one
    constructor, _GLOBAL__sub_I_onTap from tamapoke_ui.cpp, at 0x2402dc08.
    Two releases and most of a day went into hunting a black screen whose cause
    was one word in a linker script.

The cure a C++ overlay must apply -- every other one already does (lynx, a2600,
c64, tgbdual) -- is to capture its own array inside its own section:

    __init_array_<core>_start__ = .;
    KEEP(build/<core>/*.o(.init_array*))
    __init_array_<core>_end__ = .;

and call cpp_init_array(start, end) from its app entry, once the overlay is in
RAM. Then nothing of it remains in the resident array and boot is untouched.

Nothing in C, C++ or the linker warns about the omission: a global with a
constructor is ordinary code, and *(.init_array*) silently collects it. So the
check has to be external, and it has to run on every link.

usage: check_resident_init_array.py <objdump> <elf>
"""
import re
import subprocess
import sys

# RAM_EMU and the rest of AXI SRAM where overlays are linked. A resident
# constructor legitimately lives in internal flash (0x08...) or, for the very
# few that run from DTCM data, in 0x20... . 0x24... is overlay territory.
OVERLAY_LO = 0x24000000
OVERLAY_HI = 0x24100000


def tool_works(objdump, elf):
    """True if objdump can read the ELF at all.

    Probed separately from the per-section dumps on purpose: objdump exits
    non-zero for a section that simply is not there, and an earlier draft of
    this script treated that as "tool missing" and skipped -- so it reported
    SKIPPED on the very ELF it was written to catch. A gate that cannot tell
    "nothing to check" from "could not check" is not a gate.
    """
    try:
        subprocess.run([objdump, "-h", elf], capture_output=True, text=True, check=True)
        return True
    except (OSError, subprocess.CalledProcessError):
        return False


def section_words(objdump, elf, section):
    """Return the 32-bit words in `section`; empty list if the section is absent."""
    try:
        out = subprocess.run([objdump, "-s", "-j", section, elf],
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return []  # section not present in this build -- nothing to check
    words = []
    started = False
    for line in out.splitlines():
        if line.strip().startswith("Contents of section"):
            started = section in line
            continue
        if not started:
            continue
        m = re.match(r"\s*[0-9a-f]+\s+((?:[0-9a-f]{2,8}\s+)+)", line)
        if not m:
            continue
        for group in m.group(1).split():
            # objdump prints big-endian-looking groups of raw bytes; each group
            # is up to 4 bytes of the little-endian word.
            b = bytes.fromhex(group)
            for i in range(0, len(b) - 3, 4):
                words.append(int.from_bytes(b[i:i + 4], "little"))
    return words


def symbol_at(objdump, elf, addr):
    """Best-effort name for addr, for a message that names the culprit."""
    nm = objdump.replace("objdump", "nm")
    try:
        out = subprocess.run([nm, "-n", elf], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return "?"
    best = "?"
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        try:
            a = int(parts[0], 16)
        except ValueError:
            continue
        if a <= addr:
            best = parts[2]
        else:
            break
    return best


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: check_resident_init_array.py <objdump> <elf>")
    objdump, elf = sys.argv[1], sys.argv[2]

    if not tool_works(objdump, elf):
        # Skip loudly rather than fail: a safety net that breaks the build over a
        # missing toolchain teaches people to delete the safety net (CLAUDE.md).
        print(f"resident-init-array: SKIPPED (cannot run {objdump}) -- boot ctors NOT verified")
        return

    bad = []
    total = 0
    for section in (".init_array", ".preinit_array"):
        for w in section_words(objdump, elf, section):
            if w in (0, 0xFFFFFFFF):
                continue
            total += 1
            target = w & ~1  # strip the Thumb bit
            if OVERLAY_LO <= target < OVERLAY_HI:
                bad.append((section, w, target))

    if not bad:
        print(f"resident-init-array: OK ({total} boot ctor(s), none in overlay RAM)")
        return

    print("resident-init-array: FAILED -- a boot-time constructor points into overlay RAM.")
    print("  __libc_init_array() runs these before main(), when RAM_EMU holds no core.")
    print("  The device dies before the LCD is up: black screen, no BSOD, no rescue.")
    for section, word, target in bad:
        print(f"    {section}: {word:#010x} -> {target:#010x}  {symbol_at(objdump, elf, target)}")
    print("  Fix: capture that core's array inside its own overlay section --")
    print("    __init_array_<core>_start__ = .;")
    print("    KEEP(build/<core>/*.o(.init_array*))")
    print("    __init_array_<core>_end__ = .;")
    print("  and call cpp_init_array(start, end) from its app entry, as lynx/a2600/c64 do.")
    sys.exit(1)


if __name__ == "__main__":
    main()
