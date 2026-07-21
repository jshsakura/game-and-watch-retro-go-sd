#!/usr/bin/env python3
"""CPS-1 68000 static-recompiler translator -- skeleton parser/emitter.

Reads a flat binary of raw 68000 code and emits a C function translating
each linearly-decoded instruction into native C, operating on the same
register layout as Core/Src/porting/cps1/cps1_cpu68k.h (so the emitted file
can eventually replace CPS1_ENGINE_RECOMPILER's fallback-to-interpreter
path in cps1_core.c). See docs/CPS1_ULTIMATE_PORTING_PLAN.md technique 1.

Covers the SAME opcode subset the C interpreter (cps1_cpu68k.c) covers:
NOP, MOVEQ, ADDQ/SUBQ (Dn-direct), DBcc, Bcc/BRA/BSR, RTS. Anything else is
emitted as a call back into the interpreter (cps1_rc_fallback_step, see
Core/Src/porting/cps1/cps1_rc_runtime.c) -- the hybrid pattern every static
recompiler needs: translate what is understood, never guess at what isn't.

This is a LINEAR decoder, not a control-flow-graph one: it disassembles
byte-for-byte from base_address for len(code) bytes, assuming every byte in
range is reachable code. A real recompiler needs a CFG pass (trace from
entry points/vectors, follow branches, stop at data/jump tables) before
this is safe to run over a whole ROM -- that pass does not exist yet. This
script only proves the decode-table -> C-emission pipeline against the same
hand-assembled test program cps1_core.c runs (see selftest.sh).

Usage:
    python3 translate.py <input.bin> <base_address_hex> [--out out.c] [--func name]
"""
import argparse
import sys

CONDITION_NAMES = [
    "T", "F", "HI", "LS", "CC", "CS", "NE", "EQ",
    "VC", "VS", "PL", "MI", "GE", "LT", "GT", "LE",
]


class Instruction:
    def __init__(self, addr, size, mnemonic, c_lines, targets=None):
        self.addr = addr
        self.size = size
        self.mnemonic = mnemonic
        self.c_lines = c_lines
        self.targets = targets or []  # branch targets needing a C label


def sign_extend(value, bits):
    sign_bit = 1 << (bits - 1)
    return (value & (sign_bit - 1)) - (value & sign_bit)


def label_for(addr):
    return "L%06X" % addr


def decode_one(code, offset, base_address):
    """Decode a single instruction at code[offset:]. Returns an Instruction,
    or None if there aren't enough bytes left to fetch a 16-bit opcode."""
    if offset + 2 > len(code):
        return None

    addr = base_address + offset
    op = (code[offset] << 8) | code[offset + 1]

    # regs->pc/regs->cycles are kept in lockstep with cps1_cpu68k_step()'s
    # bookkeeping (same per-opcode cycle costs, same "pc = address past the
    # last fetched word" convention) so cps1_core_cpu_state_hash() compares
    # equivalent state across the interpreter and the recompiled function,
    # not just the data registers each happens to touch.

    if op == 0x4E71:
        return Instruction(addr, 2, "NOP",
                            ["regs->pc = 0x%06Xu; regs->cycles += 4;" % (addr + 2)])

    if op == 0x4E75:
        return Instruction(addr, 2, "RTS",
                            ["regs->pc = 0x%06Xu; regs->cycles += 16;" % (addr + 2),
                             "regs->halted = 1;",
                             "return;"])

    if (op & 0xF100) == 0x7000:
        reg = (op >> 9) & 7
        imm = sign_extend(op & 0xFF, 8)
        return Instruction(addr, 2, "MOVEQ #%d,D%d" % (imm, reg),
                            ["regs->d[%d] = (uint32_t)(int32_t)(%d);" % (reg, imm),
                             "cps1_rc_set_nz_flags(regs, (int32_t)(%d));" % imm,
                             "regs->pc = 0x%06Xu; regs->cycles += 4;" % (addr + 2)])

    if (op & 0xF038) == 0x5000 and ((op >> 6) & 3) != 3:
        data = (op >> 9) & 7
        if data == 0:
            data = 8
        is_sub = (op >> 8) & 1
        size = (op >> 6) & 3
        reg = op & 7
        fn = "cps1_rc_sub" if is_sub else "cps1_rc_add"
        mnem = "SUBQ" if is_sub else "ADDQ"
        return Instruction(addr, 2, "%s #%d,D%d" % (mnem, data, reg),
                            ["%s(regs, %d, %du, %d);" % (fn, reg, data, size),
                             "regs->pc = 0x%06Xu; regs->cycles += 4;" % (addr + 2)])

    if (op & 0xF0F8) == 0x50C8:
        cc = (op >> 8) & 0xF
        reg = op & 7
        if offset + 4 > len(code):
            return None
        disp = sign_extend((code[offset + 2] << 8) | code[offset + 3], 16)
        ext_addr = addr + 2
        fallthrough = ext_addr + 2  # past the extension word (= addr + 4)
        target = ext_addr + disp
        lines = [
            "if (!cps1_rc_cc_%s(regs)) {" % CONDITION_NAMES[cc],
            "    uint16_t lo = (uint16_t)((regs->d[%d] & 0xFFFF) - 1);" % reg,
            "    regs->d[%d] = (regs->d[%d] & 0xFFFF0000u) | lo;" % (reg, reg),
            "    if (lo != 0xFFFFu) {",
            "        regs->pc = 0x%06Xu; regs->cycles += 10;" % target,
            "        goto %s;" % label_for(target),
            "    }",
            "    regs->pc = 0x%06Xu; regs->cycles += 14;" % fallthrough,
            "} else {",
            "    regs->pc = 0x%06Xu; regs->cycles += 10;" % fallthrough,
            "}",
        ]
        return Instruction(addr, 4, "DB%s D%d,%s" % (CONDITION_NAMES[cc], reg, label_for(target)),
                            lines, targets=[target])

    if (op & 0xF000) == 0x6000:
        cc = (op >> 8) & 0xF
        disp8 = sign_extend(op & 0xFF, 8)
        branch_base = addr + 2
        target = branch_base + disp8
        taken = ["regs->pc = 0x%06Xu; regs->cycles += 10;" % target,
                 "goto %s;" % label_for(target)]
        if cc == 0:  # BRA -- always taken
            return Instruction(addr, 2, "BRA %s" % label_for(target), taken, targets=[target])
        if cc == 1:  # BSR -- no call-stack model yet, same TODO as the interpreter
            return Instruction(addr, 2, "BSR %s" % label_for(target),
                                ["/* TODO(cps1): BSR needs a call stack */"] + taken,
                                targets=[target])
        lines = ["if (cps1_rc_cc_%s(regs)) {" % CONDITION_NAMES[cc]]
        lines += ["    " + line for line in taken]
        lines += ["}",
                  "regs->pc = 0x%06Xu; regs->cycles += 10;" % branch_base]
        return Instruction(addr, 2, "B%s %s" % (CONDITION_NAMES[cc], label_for(target)),
                            lines, targets=[target])

    # Unimplemented: bail out to the interpreter rather than guess.
    return Instruction(addr, 2, "??? (0x%04X)" % op,
                        ["cps1_rc_fallback_step(regs, 0x%06Xu); return;" % addr])


def disassemble(code, base_address):
    instructions = []
    offset = 0
    while offset < len(code):
        insn = decode_one(code, offset, base_address)
        if insn is None:
            break
        instructions.append(insn)
        offset += insn.size
    return instructions


def emit_c(instructions, func_name):
    targets = set()
    for insn in instructions:
        targets.update(insn.targets)

    lines = [
        "/* Auto-generated by tools/cps1_recompiler/translate.py -- DO NOT EDIT BY HAND. */",
        "#include \"cps1_cpu68k.h\"",
        "#include \"cps1_rc_runtime.h\"",
        "",
        "void %s(cps1_cpu68k_t *regs)" % func_name,
        "{",
    ]
    for insn in instructions:
        if insn.addr in targets:
            lines.append("%s: ;" % label_for(insn.addr))
        lines.append("    /* 0x%06X: %s */" % (insn.addr, insn.mnemonic))
        lines.extend("    " + c_line for c_line in insn.c_lines)
    lines.append("    return;")
    lines.append("}")
    lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", help="flat binary of raw 68000 code")
    parser.add_argument("base_address", help="hex address the binary is mapped at, e.g. 0")
    parser.add_argument("--out", default=None, help="output .c path (default: stdout)")
    parser.add_argument("--func", default="cps1_rc_translated", help="emitted function name")
    args = parser.parse_args()

    with open(args.input, "rb") as f:
        code = f.read()
    base_address = int(args.base_address, 16)

    instructions = disassemble(code, base_address)
    unimplemented = sum(1 for insn in instructions if insn.mnemonic.startswith("???"))

    c_source = emit_c(instructions, args.func)

    if args.out:
        with open(args.out, "w") as f:
            f.write(c_source)
    else:
        sys.stdout.write(c_source)

    sys.stderr.write("[cps1-recomp] %d instructions decoded, %d fell back to the interpreter\n"
                      % (len(instructions), unimplemented))


if __name__ == "__main__":
    main()
