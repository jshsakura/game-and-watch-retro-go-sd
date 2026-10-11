#!/usr/bin/env python3
"""Experimental offline SH2 ALU block translation. No device or firmware writes.

Emit small, independent C functions for a Cortex-M7 compiler, rather than
replaying interpreter handlers. Only one-cycle, register-only instructions are
accepted. The generated proof relocates ROM samples into a synthetic ROM;
it establishes instruction semantics, not executable-code reachability.
"""
import argparse
import hashlib
import json
import random
from pathlib import Path


def decode(op):
    """Return direct C calculation, touched registers, dirty registers, T write."""
    n, m = (op >> 8) & 15, (op >> 4) & 15
    a, b = f'r{n}', f'r{m}'
    def result(code, touched=(n, m), dirty=(n,), flag=False):
        return code, set(touched), set(dirty), flag
    if op == 0x0009:
        return result('/* NOP */', (), ())
    if op in (0x0008, 0x0018):
        return result(f't = {int(op == 0x0018)}u;', (), (), True)
    if op >> 12 in (7, 14):
        imm = (op & 255) - (256 if op & 128 else 0)
        return result(f'{a} {"+=" if op >> 12 == 7 else "="} (uint32_t)({imm});', (n,))
    binary = {0x300c: '+', 0x3008: '-', 0x2009: '&', 0x200a: '^', 0x200b: '|'}
    key = op & 0xf00f
    if key in binary:
        return result(f'{a} = {a} {binary[key]} {b};')
    if key == 0x300e:
        return result(f'{{ uint64_t z = (uint64_t){a} + {b} + t; {a} = (uint32_t)z; t = z >> 32; }}', flag=True)
    if key == 0x300a:
        return result(f'{{ uint32_t x = {a}, y = {b}, z = x-y, c = t; {a} = z-c; t = (x < y) || (z < c); }}', flag=True)
    if key == 0x2008:
        return result(f't = (({a} & {b}) == 0);', dirty=(), flag=True)
    comparisons = {0x3000: ('==', False), 0x3002: ('>=', False),
                   0x3006: ('>', False), 0x3003: ('>=', True), 0x3007: ('>', True)}
    if key in comparisons:
        cmp, signed = comparisons[key]
        cast = '(int32_t)' if signed else ''
        return result(f't = ({cast}{a} {cmp} {cast}{b});', dirty=(), flag=True)
    unary = {
        0x6003: b, 0x6007: f'~{b}', 0x600b: f'0u - {b}',
        0x6008: f'({b} & 0xffff0000u) | (({b} & 255u) << 8) | (({b} >> 8) & 255u)',
        0x6009: f'({b} << 16) | ({b} >> 16)',
        0x600c: f'{b} & 255u', 0x600d: f'{b} & 65535u',
        0x600e: f'(uint32_t)(int32_t)(int8_t){b}',
        0x600f: f'(uint32_t)(int32_t)(int16_t){b}',
    }
    if key in unary:
        return result(f'{a} = {unary[key]};')
    key = op & 0xf0ff
    if key == 0x0029:
        return result(f'{a} = t;', (n,))
    if key == 0x4010:
        return result(f'--{a}; t = ({a} == 0); s->no_polling = SH2_NO_POLLING;', (n,), flag=True)
    shifts = {0x4008: ('<<', 2), 0x4018: ('<<', 8), 0x4028: ('<<', 16),
              0x4009: ('>>', 2), 0x4019: ('>>', 8), 0x4029: ('>>', 16)}
    if key in shifts:
        direction, count = shifts[key]
        return result(f'{a} {direction}= {count};', (n,))
    if key in (0x4000, 0x4020):
        return result(f't = {a} >> 31; {a} <<= 1;', (n,), flag=True)
    if key == 0x4001:
        return result(f't = {a} & 1u; {a} >>= 1;', (n,), flag=True)
    if key == 0x4004:
        return result(f't = {a} >> 31; {a} = ({a} << 1) | t;', (n,), flag=True)
    if key == 0x4005:
        return result(f't = {a} & 1u; {a} = ({a} >> 1) | (t << 31);', (n,), flag=True)
    raise ValueError(f'unsupported opcode {op:04x}')


def emit_function(name, pc, ops):
    decoded = [decode(op) for op in ops]
    touched = set().union(*(v[1] for v in decoded))
    dirty = set().union(*(v[2] for v in decoded))
    flag = any(v[3] for v in decoded)
    lines = [f'__attribute__((noinline)) static int {name}(SH2 *s, int cycles) {{',
             f'  if (s->pc != 0x{pc:08x}u || s->delay || s->test_irq || cycles < {len(ops)}) return 0;',
             '  uint32_t t = s->t_flag;']
    lines += [f'  uint32_t r{i} = s->r[{i}];' for i in sorted(touched)]
    lines += [f'  {v[0]}' for v in decoded]
    lines += [f'  s->r[{i}] = r{i};' for i in sorted(dirty)]
    if flag:
        lines += ['  s->t_flag = t;']
    lines += [f'  s->ppc = s->pc + {2*(len(ops)-1)}u;',
              f'  s->pc += {2*len(ops)}u;', f'  s->icount = cycles - {len(ops)};',
              '  return 1;', '}']
    return '\n'.join(lines)


def make_blocks(roms):
    rng = random.Random(0x32a07)
    # Every supported family, including register aliasing, gets a semantic test.
    families = [0x0009, 0x0008, 0x0018, 0x7123, 0xe180, 0x300c, 0x3008,
                0x300e, 0x300a, 0x2009, 0x200a, 0x200b, 0x2008, 0x3000,
                0x3002, 0x3006, 0x3003, 0x3007, 0x6003, 0x6007, 0x600b,
                0x6008, 0x6009, 0x600c, 0x600d, 0x600e, 0x600f, 0x0029,
                0x4010, 0x4008, 0x4018, 0x4028, 0x4009, 0x4019, 0x4029,
                0x4000, 0x4020, 0x4001, 0x4004, 0x4005]
    pool = []
    for op in families:
        if op in (9, 8, 24):
            pool.append(op)
        elif op >> 12 in (7, 14):
            pool += [(op & 0xf000) | (n << 8) | imm for n in (0, 1, 7, 15)
                     for imm in (0, 1, 127, 128, 255)]
        elif op >> 12 in (2, 3, 6):
            pool += [(op & 0xf00f) | (n << 8) | (m << 4)
                     for n, m in ((0, 0), (0, 1), (1, 0), (7, 15), (15, 7), (15, 15))]
        else:
            pool += [(op & 0xf0ff) | (n << 8) for n in (0, 1, 7, 15)]
    blocks = [{'kind': 'opcode_family', 'ops': [op]} for op in pool]
    for length in (4, 8, 16, 32):
        for i in range(8):
            blocks.append({'kind': 'synthetic_mixed', 'ops': rng.choices(pool, k=length)})
    for label, path in roms:
        data = path.read_bytes()
        words = [int.from_bytes(data[i:i+2], 'big') for i in range(0, len(data)-1, 2)]
        candidates, run = [], []
        for offset, op in enumerate(words):
            try:
                decode(op)
                run.append((offset*2, op))
            except ValueError:
                if len(run) >= 4:
                    candidates.append(run[:32])
                run = []
        if len(run) >= 4:
            candidates.append(run[:32])
        candidates.sort(key=lambda v: (-len(v), v[0][0]))
        for run in candidates[:16]:
            blocks.append({'kind': 'rom_static_candidate_not_reachability_verified',
                           'rom': label, 'rom_sha256': hashlib.sha256(data).hexdigest(),
                           'original_offset': run[0][0], 'ops': [v[1] for v in run]})
    return blocks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--rom', action='append', default=[], metavar='LABEL=PATH')
    parser.add_argument('--trace-candidates', type=Path,
                        help='Observed ALU runs from run_guest_trace.py; use top 32')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    roms = [(v.split('=', 1)[0], Path(v.split('=', 1)[1])) for v in args.rom]
    blocks = make_blocks(roms)
    if args.trace_candidates:
        trace_hash = hashlib.sha256(args.trace_candidates.read_bytes()).hexdigest()
        for candidate in json.loads(args.trace_candidates.read_text())[:32]:
            for op in candidate['ops']:
                decode(op)
            if not 2 <= len(candidate['ops']) <= 32:
                raise ValueError('trace ALU run length outside proof bounds')
            blocks.append({'kind': 'runtime_observed_alu', 'ops': candidate['ops'],
                           'original_pc': candidate['pc'], 'core': candidate['core'],
                           'region': candidate['region'], 'observed_count': candidate['count'],
                           'trace_candidates_sha256': trace_hash})
    rom_words = [9] * 32768
    functions, entries = [], []
    for i, block in enumerate(blocks):
        slot = 256 + i*64
        ops = block['ops']
        if slot + len(ops) > len(rom_words):
            raise ValueError('proof ROM is full')
        # Observed RAM code must exercise the interpreter's SDRAM fetch
        # route, rather than implicitly benchmarking its ROM fast path.
        region_base = 0x06000000 if block.get('region') == 'sdram' else 0x02000000
        pc = region_base + slot*2
        block.update(proof_pc=pc, function=f'native_{i}', cycles=len(ops))
        rom_words[slot:slot+len(ops)] = ops
        functions.append(emit_function(f'native_{i}', pc, ops))
        entries.append(f'  {{"{block["kind"]}_{i}", 0x{pc:08x}u, {len(ops)}, native_{i}}},')
    header = '#include <stdint.h>\n#include <cpu/sh2/sh2.h>\n'
    header += 'struct block { const char *name; uint32_t pc; int cycles; int (*run)(SH2 *, int); };\n'
    header += '\n'.join(functions)
    header += '\nstatic const struct block blocks[] = {\n' + '\n'.join(entries) + '\n};\n'
    header += '__attribute__((aligned(4))) static const uint16_t proof_rom[] = {\n'
    header += '\n'.join(','.join(hex(v) for v in rom_words[i:i+32])+','
                        for i in range(0, len(rom_words), 32)) + '\n};\n'
    (args.output/'generated.h').write_text(header)
    (args.output/'inputs.json').write_text(json.dumps({'blocks': blocks,
        'scope': 'one-cycle register-only blocks; ROM samples relocated; no game coverage or device speed claim'}, indent=2)+'\n')
    print(json.dumps({'status': 'generated', 'blocks': len(blocks), 'output': str(args.output)}))


if __name__ == '__main__':
    main()
