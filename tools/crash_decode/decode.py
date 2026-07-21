#!/usr/bin/env python3
"""Generic Game & Watch crash (BSOD) decoder.

Turns a bare on-device fault dump — "Hardfault  PC=0x.. LR=0x.." and nothing
else — into a symbol, file:line and cause classification, WITHOUT any manual
address arithmetic. It exists because every crash on this target lands in one
of three address spaces that a plain addr2line cannot resolve on its own:

  1. Resident intflash (0x08xxxxxx) ............ direct addr2line, easy.
  2. Overlay RAM  (0x24025800..0x24100000) ..... EVERY core's overlay is linked
     at the SAME VMA, so a raw 0x24xxxxxx address aliases — nm/addr2line return
     whichever core they hit first (usually the wrong one). We disambiguate by
     the ACTIVE core (parsed from the dump) and the .map's per-object owner.
  3. Runtime XIP flash (0x9xxxxxxx) ............. a core's cold code/rodata is
     linked at a sentinel base (SEGACD_CODE=0xDEC80000, SM=0xDEAD0000, ...) and
     copied to a *runtime* flash address at boot. addr2line knows only the
     sentinel VMA. We reverse the relocation:
         static = runtime - xip_blob_base + CODE_BASE
     using the "xip blob at 0x.." line the firmware already prints.

Usage:
    tools/crash_decode/decode.py build/gw_retro_go.elf < paste.txt
    tools/crash_decode/decode.py build/gw_retro_go.elf --pc 0 --lr 0x926a208b \
        --xip-base 0x926a2000 --core segacd

Anything the dump text already contains (PC=, LR=, "xip blob at 0x..", the core
name, CFSR=) is auto-extracted; CLI flags override. Reads the ELF's own linker
script for the XIP registry when available, else falls back to the table below.
"""
import argparse, os, re, subprocess, sys

# Fallback XIP sentinel registry (name-token -> CODE_BASE). Authoritative source
# is the linker script; this is only used if it can't be located.
XIP_FALLBACK = {
    "pico8":  0xBEEF0000,
    "sm":     0xDEAD0000,   # Super Metroid
    "gba":    0xDEC00000,
    "segacd": 0xDEC80000,
}
# Core name tokens as they appear in a dump -> registry key.
CORE_TOKENS = {
    "segacd": "segacd", "sega cd": "segacd", "mega cd": "segacd",
    "super metroid": "sm", "sm ": "sm", ".xip_sm": "sm",
    "gpsp": "gba", "gba": "gba",
    "pico-8": "pico8", "pico8": "pico8", "pico 8": "pico8",
}
RAM_EMU_DEFAULT = (0x24025800, 0x24100000)   # overridden from nm if ELF has it


def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def tool(name):
    for cand in (f"arm-none-eabi-{name}", name):
        if sh(["which", cand]).strip():
            return cand
    return f"arm-none-eabi-{name}"


def parse_xip_registry(elf):
    """Prefer the real linker script next to the ELF; fall back to the table."""
    reg = dict(XIP_FALLBACK)
    for ld in ("STM32H7B0VBTx_SDCARD.ld", "STM32H7B0VBTx_FLASH.ld"):
        if os.path.exists(ld):
            for m in re.finditer(r"(\w+)_CODE\s*\(x\)\s*:\s*ORIGIN\s*=\s*(0x[0-9A-Fa-f]+)",
                                  open(ld).read()):
                key = m.group(1).lower()
                key = {"pico8": "pico8", "sm": "sm", "gba": "gba", "segacd": "segacd"}.get(key, key)
                reg[key] = int(m.group(2), 16)
            break
    return reg


def ram_emu_range(elf, nm):
    lo = hi = None
    for line in sh([nm, elf]).splitlines():
        p = line.split()
        if len(p) >= 3 and p[2] == "__RAM_EMU_START__":
            lo = int(p[0], 16)
        elif len(p) >= 3 and p[2] == "__RAM_EMU_END__":
            hi = int(p[0], 16)
    return (lo, hi) if lo and hi else RAM_EMU_DEFAULT


def extract(text):
    """Pull PC, LR, xip base, core, CFSR out of a pasted dump. Tolerant of the
    device's mojibake / line wraps."""
    def hexof(pat):
        m = re.search(pat, text, re.I)
        return int(m.group(1), 16) if m else None
    got = {
        "pc":  hexof(r"PC\s*=\s*(0x[0-9A-Fa-f]+)"),
        "lr":  hexof(r"LR\s*=\s*(0x[0-9A-Fa-f]+)"),
        "xip": hexof(r"xip blob at\s*(0x[0-9A-Fa-f]+)"),
        "cfsr": hexof(r"CFSR\s*=\s*((?:0x)?[0-9A-Fa-f]{4,8})"),
        "abfsr": hexof(r"ABFSR\s*=\s*((?:0x)?[0-9A-Fa-f]{4,8})"),
        "core": None,
    }
    low = text.lower()
    for tok, key in CORE_TOKENS.items():
        if tok in low:
            got["core"] = key
            break
    return got


def addr2line(a2l, elf, addr):
    out = sh([a2l, "-f", "-i", "-e", elf, hex(addr)]).strip().splitlines()
    # pairs of (func, file:line); -i may add inlined frames
    frames = []
    for i in range(0, len(out) - 1, 2):
        frames.append((out[i], out[i + 1]))
    return frames


def owner_object(mapfile, sym):
    """Which build/<core>/<file>.o defines `sym` (disambiguates overlay alias)."""
    if not mapfile or not os.path.exists(mapfile) or not sym or sym == "??":
        return None
    txt = open(mapfile, errors="ignore").read()
    # ld map: a symbol line often has the .o path on the same/next token run
    m = re.search(r"\b" + re.escape(sym) + r"\b[^\n]*?(build/\S+\.o)", txt)
    if not m:
        m = re.search(r"(build/\S+\.o)\s*\n[^\n]*\b" + re.escape(sym) + r"\b", txt)
    return m.group(1) if m else None


def classify(addr, ram_lo, ram_hi):
    if addr is None:
        return "none", None
    if addr < 0x1000:
        return "null", None
    if 0x08000000 <= addr < 0x08200000:
        return "intflash", None            # resident code, direct lookup
    if ram_lo <= addr < ram_hi:
        return "overlay", None             # aliased across cores
    if 0x90000000 <= addr < 0xA0000000:
        return "xip", None                 # runtime flash cache (needs un-reloc)
    if 0x24000000 <= addr < 0x24100000:
        return "dtcm/ram", None
    return "other", None


def decode_one(label, addr, kind, info):
    a2l, elf, reg, xip_base, core, mapfile, ram = info
    print(f"\n{label} = {hex(addr) if addr is not None else '(none)'}")
    if addr is None:
        return
    static = addr
    note = ""
    if kind == "null":
        print("  ► NULL / near-null — indirect call or load through an "
              "uninitialized pointer. The OTHER register's function holds the "
              "faulting `(*fp)()` / deref site.")
        return
    if kind == "xip":
        if not xip_base or not core or core not in reg:
            print("  ► XIP runtime address but no (xip-base, core) to reverse it."
                  "\n    Pass --xip-base 0x.. --core <segacd|sm|gba|pico8>, or include"
                  "\n    the firmware's 'xip blob at 0x..' line and the core name.")
            return
        static = addr - xip_base + reg[core]
        note = f"  (un-relocated: {hex(addr)} - {hex(xip_base)} + {hex(reg[core])} = {hex(static)})"
        print(note)
    frames = addr2line(a2l, elf, static)
    if not frames or frames[0][0] == "??":
        print("  ► no symbol (addr not in this ELF — wrong build? or a data addr)")
        return
    for j, (fn, loc) in enumerate(frames):
        tag = "   inlined by" if j else "  ►"
        print(f"{tag} {fn}   {loc}")
    if kind == "overlay":
        owner = owner_object(mapfile, frames[-1][0])
        if owner:
            print(f"    owner: {owner}")
            if core and f"/{core}/" not in owner:
                print(f"    ⚠ overlay VMA aliases — this symbol is owned by "
                      f"{owner}, but the active core looks like '{core}'. The real "
                      f"culprit is {core}'s object at the same VMA; resolve against "
                      f"build/{core}/*.o.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--pc", type=lambda x: int(x, 0))
    ap.add_argument("--lr", type=lambda x: int(x, 0))
    ap.add_argument("--xip-base", type=lambda x: int(x, 0))
    ap.add_argument("--core")
    ap.add_argument("--cfsr", type=lambda x: int(x, 0))
    ap.add_argument("--abfsr", type=lambda x: int(x, 0))
    ap.add_argument("--map")
    args = ap.parse_args()

    text = "" if sys.stdin.isatty() else sys.stdin.read()
    g = extract(text)
    pc  = args.pc  if args.pc  is not None else g["pc"]
    lr  = args.lr  if args.lr  is not None else g["lr"]
    xip = args.xip_base if args.xip_base is not None else g["xip"]
    core = args.core or g["core"]
    cfsr = args.cfsr if args.cfsr is not None else g["cfsr"]
    abfsr = args.abfsr if args.abfsr is not None else g["abfsr"]
    mapfile = args.map or (args.elf[:-4] + ".map" if args.elf.endswith(".elf") else None)

    nm  = tool("nm")
    a2l = tool("addr2line")
    reg = parse_xip_registry(args.elf)
    ram = ram_emu_range(args.elf, nm)

    print("=" * 64)
    print(f"crash decode  |  core={core or '?'}  xip_base={hex(xip) if xip else '?'}"
          f"  RAM_EMU=[{hex(ram[0])},{hex(ram[1])})")
    print("=" * 64)

    if cfsr is not None:
        decode_cfsr(cfsr)
    if abfsr:
        decode_abfsr(abfsr)

    info = (a2l, args.elf, reg, xip, core, mapfile, ram)
    for label, addr in (("PC", pc), ("LR", lr)):
        kind, _ = classify(addr, *ram)
        decode_one(label, addr, kind, info)
    print()


CFSR_BITS = [
    # (bit, name, hint)
    (0,  "IACCVIOL",  "MemManage: instruction fetch from a no-execute region"),
    (1,  "DACCVIOL",  "MemManage: data access to a protected region"),
    (3,  "MUNSTKERR", "MemManage: fault unstacking on exception return"),
    (4,  "MSTKERR",   "MemManage: fault stacking on exception entry"),
    (7,  "MMARVALID", "MMFAR holds the faulting data address"),
    (8,  "IBUSERR",   "BusFault: instruction prefetch"),
    (9,  "PRECISERR", "BusFault: precise data bus error — BFAR is the address"),
    (10, "IMPRECISERR","BusFault: imprecise (buffered store) — PC is drain-time noise, "
                       "read ABFSR (0xE000EFA8) for the bus: bit2=AHBP(peripheral) "
                       "bit3=AXIM(RAM/flash) bit0/1=ITCM/DTCM"),
    (11, "UNSTKERR",  "BusFault on exception return unstacking"),
    (12, "STKERR",    "BusFault on exception entry stacking"),
    (15, "BFARVALID", "BFAR holds the faulting bus address"),
    (16, "UNDEFINSTR","UsageFault: undefined instruction"),
    (17, "INVSTATE",  "UsageFault: invalid state — e.g. branch to an address with "
                      "Thumb bit clear (a NULL/garbage function pointer)"),
    (18, "INVPC",     "UsageFault: invalid PC on exception return"),
    (19, "NOCP",      "UsageFault: coprocessor access (FPU not enabled?)"),
    (24, "UNALIGNED", "UsageFault: unaligned access — on M7 a 64-bit STRD/LDRD to a "
                      "non-word-aligned address traps here"),
    (25, "DIVBYZERO", "UsageFault: divide by zero"),
]


ABFSR_BITS = [
    (0, "ITCM",  "instruction/data TCM"),
    (1, "DTCM",  "data TCM"),
    (2, "AHBP",  "AHBP — peripheral space 0x40000000-0x5FFFFFFF (a wild MMIO access)"),
    (3, "AXIM",  "AXIM — all RAM/flash (AXI SRAM, external flash, QSPI)"),
    (4, "EPPB",  "external private peripheral bus"),
]


def decode_abfsr(abfsr):
    print(f"\nABFSR = {hex(abfsr)}  (which bus the imprecise fault used)")
    hit = [(n, h) for (b, n, h) in ABFSR_BITS if abfsr & (1 << b)]
    if not hit:
        print("  (no bus bit set — fault may not have been an imprecise bus error)")
    for n, h in hit:
        print(f"  ► {n}: {h}")
    if abfsr & 0xE0:
        print(f"    (TYPE field = {(abfsr >> 5) & 7})")


def decode_cfsr(cfsr):
    print(f"\nCFSR = {hex(cfsr)}")
    hit = [(n, h) for (b, n, h) in CFSR_BITS if cfsr & (1 << b)]
    if not hit:
        print("  (no known bits set)")
    for n, h in hit:
        print(f"  ► {n}: {h}")


if __name__ == "__main__":
    main()
