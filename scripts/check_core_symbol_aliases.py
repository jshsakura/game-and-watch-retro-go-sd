#!/usr/bin/env python3
"""Every emulator core is an overlay, and every overlay is linked at the SAME RAM
address. So if core A references a global that only core B defines, the linker
resolves it — silently, no warning — to an address that, once A is loaded, holds
A's own unrelated data. A reads garbage and jumps into nowhere.

This is exactly how the Super Metroid port died on its first boot: sm_cpu_infra.c
(which defined g_snes) was left out of the build, g_snes was not in sm_redefines,
and the linker bound sm's entire SNES bus to Super Mario World's g_snes. It linked
clean, and it asserted on the first register read.

The cure is that a core's globals must be renamed into its own namespace by its
<core>_redefines file, so a missing definition is a link ERROR rather than an
alias. This checks that no core still has one.

Only LIVE references count. A reference inside code that --gc-sections threw away
is harmless, and the .o files cannot tell the difference — so the check confirms
the alias by disassembling the offending core's overlay and looking for an actual
branch to the foreign symbol's address. (zelda3 names smw's snes_read but never
calls it; that is not a bug.)

Usage: check_core_symbol_aliases.py <build_dir> <elf>
"""
import glob
import os
import subprocess
import sys

def _find_nm():
    """The Makefile passes NM=$(PREFIX)nm, but not every CI image has the toolchain
    on a non-login PATH — and the SD_CARD=0 job does not. This check is a safety
    net, not a build step: if nm cannot be found, say so loudly and let the build
    through rather than failing it on a missing tool."""
    import shutil
    for cand in (os.environ.get("NM"), "arm-none-eabi-nm", "llvm-nm"):
        if cand and (shutil.which(cand) or os.path.isfile(cand)):
            return cand
    return None


NM = _find_nm()
if NM is None:
    print("check_core_symbol_aliases: no nm on PATH — skipping the cross-overlay check",
          file=sys.stderr)
    sys.exit(0)

# Directories under build/ that are emulator-core overlays. Everything else
# (core/, fatfs/, tamp/, ...) is resident firmware and is shared on purpose.
# rc_smw is the old SNES static-recompilation XIP blob; rc_smw_hot is its
# ITCM-resident hot-subset replacement. Both are compiled with snes_redefines
# and belong to the SNES (gsnes__) namespace. Neither is a standalone overlay,
# so treating their object directories as cores creates false cross-core alias
# reports against the SNES overlay they intentionally reference.
NON_CORE_DIRS = {
    "core", "cores", "fatfs", "tamp", "mappers", "rc_smw", "rc_smw_hot"
}

DEFINED_TYPES = "BbDdTtGgRrVvWw"


def symbols(obj):
    """(referenced-but-undefined, defined) for one object file."""
    undef, defined = set(), set()
    out = subprocess.run([NM, obj], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] == "U":
            undef.add(parts[1])
        elif len(parts) == 3 and parts[1] in DEFINED_TYPES:
            defined.add(parts[2])
    return undef, defined


def main():
    build_dir, elf = sys.argv[1], sys.argv[2]

    cores = sorted(
        d for d in os.listdir(build_dir)
        if os.path.isdir(os.path.join(build_dir, d)) and d not in NON_CORE_DIRS
    )
    if not cores:
        print("no core directories found — nothing to check")
        return 0

    # What each core defines, and what each core leaves undefined.
    defines, undefines = {}, {}
    for core in cores:
        objs = glob.glob(os.path.join(build_dir, core, "*.o"))
        if not objs:
            continue
        u, d = set(), set()
        for o in objs:
            ou, od = symbols(o)
            u |= ou
            d |= od
        defines[core], undefines[core] = d, u

    owner = {}
    for core, d in defines.items():
        for s in d:
            owner.setdefault(s, set()).add(core)

    # Address of each symbol that made it into the image.
    addr = {}
    for line in subprocess.run([NM, elf], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in DEFINED_TYPES:
            addr[parts[2]] = parts[0]

    objdump = NM.replace("nm", "objdump")
    disasm = {}

    def overlay_text(core):
        """Disassembly of this core's overlay, or '' if it has no overlay section.

        Some cores (SegaCD, Super Metroid, GBA — see SEGACD_CODE/SM_CODE/GBA_CODE
        in the linker script) keep cold code+rodata out of RAM_EMU via a sentinel
        XIP section (.xip_<core>) instead of .overlay_<core>. A call site that
        lives there is just as real as one in the RAM overlay, so it must be
        checked too — 0720: this is exactly why check_core_symbol_aliases missed
        the SegaCD gwenesis_io_get_buttons alias (its call site is in
        .xip_segacd), and the same blind spot applies to sm/gba."""
        if core not in disasm:
            sections = [f"--section=.overlay_{core}", f"--section=.xip_{core}"]
            r = subprocess.run(
                [objdump, "-d"] + sections + [elf],
                capture_output=True, text=True)
            disasm[core] = r.stdout
        return disasm[core]

    failures = []
    for core in sorted(undefines):
        for s in sorted(undefines[core] - defines[core]):
            foreign = owner.get(s, set()) - {core}
            if not foreign or s not in addr:
                continue
            # A real alias means this core actually reaches the symbol. objdump
            # annotates both a branch target and a literal-pool word with the name
            # it resolved to ("bl 240aaaa0 <snes_read>"), so that is what to look
            # for — the bare address turns up by coincidence far too often.
            #
            # A direct "bl <addr> <sym>" is not the only shape this takes: a
            # call from XIP-relocated code (SEGACD_CODE/SM_CODE/GBA_CODE, ~2MB+
            # away from RAM_EMU) is out of BL range, so the linker inserts an
            # interworking veneer ("bl ... <__sym_veneer>") that loads the real
            # address from a literal pool instead. The veneer itself lives in
            # the same XIP section and is named after the symbol it targets, so
            # that name is just as reliable a live-reference marker as the
            # direct-branch annotation — 0720: this is the second (and the
            # decisive) reason the SegaCD gwenesis_io_get_buttons alias slipped
            # through even after XIP sections were added to overlay_text().
            text = overlay_text(core)
            if f"{addr[s]} <{s}>" in text or f"<__{s}_veneer>" in text:
                failures.append((core, s, sorted(foreign)))

    if failures:
        print("FAIL: a core references a global that another core's overlay owns.")
        print("      Both live at the same RAM address; the reader gets garbage.\n")
        for core, sym, foreign in failures:
            print(f"  {core:<10} -> {sym:<32} defined only in: {', '.join(foreign)}")
        print(f"\n  Fix: add the symbol to {failures[0][0]}_redefines (so it becomes")
        print("  <core>__<symbol> and cannot alias), and define it in that core.")
        return 1

    print(f"OK  {len(cores)} cores, no cross-overlay symbol aliases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
