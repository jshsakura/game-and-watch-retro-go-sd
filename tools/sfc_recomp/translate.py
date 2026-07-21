#!/usr/bin/env python3
"""65816 -> C static translator (PoC).

Reads: the interpreter (cpu.c), the executed-site list (from the -DSNES_PC_HISTOGRAM
dump run), and the live expanded cart image (SNES_CARTDUMP). Emits rc_sites.inc:
one C function per executed ROM instruction site, with the opcode/operand FETCHES
constant-folded (their snes_cpuRead side effects — cpuMemOps++/cpuCyclesLeft+=8 —
replicated exactly) and the opcode BODY spliced verbatim from cpu.c's own switch,
so semantics are the interpreter's by construction. Data accesses still call the
real bus. One instruction per call — the event loop charges cycles and fires
events between opcodes, so whole-block fusion would shift NMI/IRQ/DMA timing and
break the bit-identical gate.

Usage: translate.py <cpu.c> <sites.bin> <cart.bin> <type> <romMask> <out.inc>
"""
import re
import struct
import sys
from collections import defaultdict


def parse_cases(cpu_src):
    """Extract each `case 0xNN: { body }` from cpu_doOpcode's switch."""
    start = cpu_src.index("static void cpu_doOpcode(Cpu* cpu, uint8_t opcode)")
    cases = {}
    for m in re.finditer(r"case 0x([0-9a-fA-F]{2}): \{", cpu_src[start:]):
        op = int(m.group(1), 16)
        i = start + m.end()  # after the '{'
        depth = 1
        j = i
        while depth:
            c = cpu_src[j]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
            j += 1
        body = cpu_src[i:j - 1]
        # strip the trailing 'break;' (not meaningful outside the switch)
        body = re.sub(r"\bbreak;\s*$", "", body.rstrip())
        cases[op] = body
    return cases


# addressing-mode transforms: pattern -> (replacement-template, operand bytes consumed)
# {b}=1-byte const, {w}=2-byte const, {l}=3-byte const hex literals.
MODE_XFORMS = [
    ("cpu_adrImm(cpu, &low, false)", None, 2),   # kept verbatim (no fetch inside), max 2 bytes
    ("cpu_adrImm(cpu, &low, true)",  None, 2),
    ("cpu_adrIdy(cpu, &low, false)", "rc_adrIdy(cpu, &low, false, {b})", 1),
    ("cpu_adrIdy(cpu, &low, true)",  "rc_adrIdy(cpu, &low, true, {b})", 1),
    ("cpu_adrAbx(cpu, &low, false)", "rc_adrAbx(cpu, &low, false, {w})", 2),
    ("cpu_adrAbx(cpu, &low, true)",  "rc_adrAbx(cpu, &low, true, {w})", 2),
    ("cpu_adrAby(cpu, &low, false)", "rc_adrAby(cpu, &low, false, {w})", 2),
    ("cpu_adrAby(cpu, &low, true)",  "rc_adrAby(cpu, &low, true, {w})", 2),
    ("cpu_adrDpx(cpu, &low)", "rc_adrDpx(cpu, &low, {b})", 1),
    ("cpu_adrDpy(cpu, &low)", "rc_adrDpy(cpu, &low, {b})", 1),
    ("cpu_adrIdp(cpu, &low)", "rc_adrIdp(cpu, &low, {b})", 1),
    ("cpu_adrIdx(cpu, &low)", "rc_adrIdx(cpu, &low, {b})", 1),
    ("cpu_adrIdl(cpu, &low)", "rc_adrIdl(cpu, &low, {b})", 1),
    ("cpu_adrIly(cpu, &low)", "rc_adrIly(cpu, &low, {b})", 1),
    ("cpu_adrIsy(cpu, &low)", "rc_adrIsy(cpu, &low, {b})", 1),
    ("cpu_adrAbl(cpu, &low)", "rc_adrAbl(cpu, &low, {l})", 3),
    ("cpu_adrAlx(cpu, &low)", "rc_adrAlx(cpu, &low, {l})", 3),
    ("cpu_adrAbs(cpu, &low)", "rc_adrAbs(cpu, &low, {w})", 2),
    ("cpu_adrDp(cpu, &low)",  "rc_adrDp(cpu, &low, {b})", 1),
    ("cpu_adrSr(cpu, &low)",  "rc_adrSr(cpu, &low, {b})", 1),
    ("cpu_adrIax(cpu)",       "rc_adrIax(cpu, {w})", 2),
    ("cpu_readOpcodeWord(cpu)", "rc_fetch16(cpu, {w})", 2),
    ("cpu_readOpcode(cpu)",     "rc_fetch8(cpu, {b})", 1),
]

FALLBACK_OPS = {0x00}  # brk: CpuOpcodeHook + goto restart


def max_consumption(body):
    """Worst-case operand bytes a body consumes (for the bank-wrap guard)."""
    total = 0
    pos = 0
    while pos < len(body):
        hit = None
        for pat, _, nbytes in MODE_XFORMS:
            k = body.find(pat, pos)
            if k >= 0 and (hit is None or k < hit[0]):
                hit = (k, pat, nbytes)
        if hit is None:
            break
        total += hit[2]
        pos = hit[0] + len(hit[1])
    return total


def has_fetch(text):
    return any(pat in text for pat, _, _ in MODE_XFORMS)


def transform_body(body, operands):
    """Replace fetch/addressing calls left-to-right with constant-folded forms.

    Textual order == execution order for every body in cpu.c's switch EXCEPT
    opcode 0x89 (biti imm): its 8-bit and 16-bit fetches sit in mutually
    exclusive if/else arms, so both arms must fold from operand byte 0 — a
    linear cursor bakes the 16-bit immediate from bytes +2/+3 instead of +1/+2
    (caught by -DRC_VERIFY on Super Metroid). Fold each arm independently.
    Returns None if a fetch call survives (unknown pattern -> fallback)."""
    if "} else {" in body:
        i = body.index("} else {")
        if has_fetch(body[:i]) and has_fetch(body[i:]):
            a = transform_body(body[:i], operands)
            b = transform_body(body[i:], operands)
            return None if a is None or b is None else a + b
    out = []
    pos = 0
    cur = 0  # operand-byte cursor
    while pos < len(body):
        hit = None
        for pat, repl, nbytes in MODE_XFORMS:
            k = body.find(pat, pos)
            if k >= 0 and (hit is None or k < hit[0]):
                hit = (k, pat, repl, nbytes)
        if hit is None:
            out.append(body[pos:])
            break
        k, pat, repl, nbytes = hit
        out.append(body[pos:k])
        if repl is None:            # adrImm: keep verbatim, consumes at runtime
            out.append(pat)
        else:
            b = operands[cur] if nbytes >= 1 else 0
            w = (operands[cur] | (operands[cur + 1] << 8)) if nbytes >= 2 else 0
            l = (w | (operands[cur + 2] << 16)) if nbytes >= 3 else 0
            out.append(repl.format(b="0x%02x" % b, w="0x%04x" % w, l="0x%06x" % l))
        cur += nbytes
        pos = k + len(pat)
    text = "".join(out)
    if "cpu_readOpcode" in text or "goto restart" in text or "CpuOpcodeHook" in text:
        return None
    return text


def main():
    cpu_c, sites_bin, cart_bin, cart_type, rom_mask, out_inc = sys.argv[1:7]
    cart_type = int(cart_type)
    rom_mask = int(rom_mask, 0)
    cpu_src = open(cpu_c).read()
    cart = open(cart_bin, "rb").read()
    raw = open(sites_bin, "rb").read()
    sites = sorted(struct.unpack("<%dI" % (len(raw) // 4), raw))
    cases = parse_cases(cpu_src)
    assert len(cases) == 256, "expected 256 cases, got %d" % len(cases)

    def cart_fold(addr, size):
        """Mirror of cart.c's cart_fold(): non-power-of-2 images (romMask==0
        since sm 94869fb keeps the raw image instead of a pow2 expansion) fold
        out-of-range addresses onto the last power-of-2 chunk, exactly like the
        cart's chip-select decoding."""
        if size == 0:
            return 0
        base, mask = 0, 1 << 31
        while addr >= size:
            while not (addr & mask):
                mask >>= 1
            addr -= mask
            if size > mask:
                size -= mask
                base += mask
        return base + addr

    def rom_byte(bank, off):
        if cart_type == 1:
            idx = ((bank & 0x7F) << 15) | (off & 0x7FFF)
        else:
            idx = ((bank & 0x3F) << 16) | off
        # cart.c cart_romIndex(): one AND for power-of-2 images, fold otherwise
        idx = (idx & rom_mask) if rom_mask else cart_fold(idx, len(cart))
        return cart[idx]

    def is_rom_site(a):
        bank = a >> 16
        off = a & 0xFFFF
        return bank not in (0x7E, 0x7F) and off >= 0x8000

    emitted = []       # (addr, fnname)
    skipped = defaultdict(int)
    fns = []
    for a in sites:
        bank, off = a >> 16, a & 0xFFFF
        if not is_rom_site(a):
            skipped["non-ROM site"] += 1
            continue
        op = rom_byte(bank, off)
        if op in FALLBACK_OPS:
            skipped["fallback opcode 0x%02x" % op] += 1
            continue
        body = cases[op]
        need = max_consumption(body)
        if off + 1 + need > 0x10000:
            skipped["bank-wrap"] += 1
            continue
        operands = [rom_byte(bank, off + 1 + i) for i in range(need)]
        text = transform_body(body, operands)
        if text is None:
            skipped["untransformable 0x%02x" % op] += 1
            continue
        fn = "rc_s_%06x" % a
        fns.append(
            "static void %s(Cpu *cpu) { /* %02x:%04x op %02x */\n"
            "  rc_fetch8(cpu, 0x%02x);\n"
            "  cpu->cyclesUsed = cyclesPerOpcode[0x%02x];\n"
            "  {%s}\n"
            "}\n" % (fn, bank, off, op, op, op, text))
        emitted.append((a, fn, need))

    # FNV-1a 32-bit hash of consumed bytes (opcode + operands) at all sites.
    # This is the "code-region hash" gate: if these bytes match, the sites are
    # provably correct for this ROM (text/graphics patches don't affect code).
    FNV_OFFSET = 0x811C9DC5
    FNV_PRIME  = 0x01000193
    code_hash = FNV_OFFSET
    for a, _, need in emitted:
        bank, off = a >> 16, a & 0xFFFF
        for i in range(1 + need):
            code_hash ^= rom_byte(bank, off + i)
            code_hash = (code_hash * FNV_PRIME) & 0xFFFFFFFF

    # RC_HASH_CAP: total open-addressing hash slots across all banks.
    # Each bank with N sites gets next_pow2(N*2) slots (LF~0.5).
    # The dispatch table lives in overlay BSS — this is the exact slot count
    # the caller must reserve. ~85 KB for SMW (8371 sites, 7 banks).
    bank_counts = defaultdict(int)
    for a, _, _ in emitted:
        bank_counts[a >> 16] += 1
    rc_hash_cap = 0
    for c in bank_counts.values():
        sz = 1
        while sz < c * 2:
            sz <<= 1
        rc_hash_cap += sz

    with open(out_inc, "w") as f:
        f.write("/* AUTO-GENERATED by translate.py — one native fn per executed ROM site. */\n")
        f.write("".join(fns))
        f.write("\n#define RC_NSITES %d\n" % len(emitted))
        f.write("#define RC_CODE_HASH 0x%08xU\n" % code_hash)
        f.write("#define RC_HASH_CAP %d\n" % rc_hash_cap)
        f.write("static void (*const rc_fns[RC_NSITES])(Cpu*) = {\n")
        for _, fn, _ in emitted:
            f.write("  %s,\n" % fn)
        f.write("};\n")
        f.write("static const uint32_t rc_addrs[RC_NSITES] = {\n")
        for a, _, _ in emitted:
            f.write("  0x%06x,\n" % a)
        f.write("};\n")
        f.write("/* Per-site consumed byte count (operand bytes, NOT including opcode byte). */\n")
        f.write("/* Total consumed per site = 1 (opcode) + rc_site_lens[i]. */\n")
        f.write("static const uint8_t rc_site_lens[RC_NSITES] = {\n")
        for _, _, need in emitted:
            f.write("  %d,\n" % need)
        f.write("};\n")

    print("translated %d sites; code_hash=0x%08x; skipped: %s" %
          (len(emitted), code_hash, dict(skipped) or "none"), file=sys.stderr)
    ops = defaultdict(int)
    for a, _, _ in emitted:
        ops[rom_byte(a >> 16, a & 0xFFFF)] += 1
    top = sorted(ops.items(), key=lambda kv: -kv[1])[:10]
    print("top opcodes: %s" % ", ".join("%02x:%d" % kv for kv in top), file=sys.stderr)


if __name__ == "__main__":
    main()
