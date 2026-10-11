#!/usr/bin/env python3
"""Finite After Burner guest execution census in an isolated M7 QEMU core.

Census mode runs without icount; its timing output is invalid. Performance
mode disables the observer and uses the existing M7 icount meter. No hardware
access, no status polling.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import struct
import sys
import time
from collections import Counter
from native_blocks import decode
import native_math
import merged_dispatch
import native_selection

ROOT = Path(__file__).resolve().parents[2]


def region(pc):
    a = pc & ~0x20000000
    if 0x02000000 <= a < 0x02400000:
        return 'rom'
    if 0x06000000 <= a < 0x06040000:
        return 'sdram'
    if 0xc0000000 <= a < 0xc0001000:
        return 'data_array'
    return 'other'


def supported(op):
    try:
        decode(op)
        return True
    except ValueError:
        return False


def analyze_memory(log, out):
    dropped = re.search(r'^MEM_DROPPED (\d+)$', log, re.M)
    if not dropped:
        return None
    if 'MEM_END' not in log or int(dropped[1]):
        raise ValueError('Incomplete memory-site diagnostic')
    sites = []
    totals = Counter()
    for line in log.splitlines():
        if not line.startswith('MEM_SITE '):
            continue
        v = line.split()[1:]
        core, pc, op = int(v[0]), int(v[1],16), int(v[2],16)
        write, width, lo, hi, count = int(v[3]), int(v[4]), int(v[5],16), int(v[6],16), int(v[7])
        a = lo & ~0x20000000
        area = 'framebuffer' if 0x04000000 <= a < 0x04040000 else region(lo)
        totals[(area, 'write' if write else 'read')] += count
        sites.append(dict(core=core, pc=pc, opcode=op, write=bool(write), width=width,
                          address_min=lo, address_max=hi, count=count, region=area))
    poll = re.search(r'^POLL ([0-9a-f]+) ([0-9a-f]+) (\d+) (\d+) (\d+) (\d+) (\d+)$', log, re.M)
    values = [{'value':int(a,16),'count':int(n)} for a,n in re.findall(r'^POLL_VALUE ([0-9a-f]+) (\d+)$',log,re.M)]
    value_dropped = re.search(r'^POLL_VALUE_DROPPED (\d+)$',log,re.M)
    if not poll or not value_dropped or int(value_dropped[1]):
        raise ValueError('Incomplete poll-value diagnostic')
    p = poll.groups()
    if sum(v['count'] for v in values) != int(p[2]) or int(p[3])+int(p[4]) != int(p[2]):
        raise ValueError('Poll-value counts do not match reads')
    result = {'status':'complete_memory_address_diagnostic', 'dropped_sites':int(dropped[1]),
              'sites':sorted(sites,key=lambda s:s['count'],reverse=True),
              'region_accesses':[dict(region=a,direction=b,count=n) for (a,b),n in totals.items()],
              'poll':dict(address_min=int(p[0],16),address_max=int(p[1],16),reads=int(p[2]),
                          zero_reads=int(p[3]),nonzero_reads=int(p[4]),counter_min=int(p[5]),counter_max=int(p[6]),values=values),
              'limitations':['Only the selected memory kernels and read loop are observed.',
                             'Effective addresses and poll results are observed without extra guest reads.',
                             'No device timing, poll acceleration or memory translation implemented.']}
    (out/'memory-result.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


def analyze(log, out):
    cores = []
    runs = []
    for line in log.splitlines():
        if line.startswith('TRACE_CORE '):
            v = list(map(int, line.split()[1:]))
            cores.append(dict(zip(['core', 'observed', 'rom', 'sdram', 'data_array', 'other'], v)))
        if line.startswith('TRACE_RUN '):
            v = line.split()[1:]
            ops = [int(op, 16) for op in v[4:]]
            runs.append({'core': int(v[0]), 'pc': int(v[1], 16), 'delay': int(v[2]),
                         'count': int(v[3]), 'ops': ops, 'region': region(int(v[1], 16))})
    dropped = re.search(r'^TRACE_DROPPED (\d+)$', log, re.M)
    if 'TRACE_END' not in log or len(cores) != 2 or not dropped:
        raise ValueError('Incomplete trace; no coverage claim permitted')
    observed = sum(c['observed'] for c in cores)
    counted = sum(r['count'] * len(r['ops']) for r in runs)
    if int(dropped[1]) or counted != observed:
        raise ValueError(f'Trace lost instructions: observed={observed}, counted={counted}, dropped={dropped[1]}')
    alu = sum(r['count']*sum(supported(op) for op in r['ops']) for r in runs)
    eligible = {}
    for r in runs:
        if r['delay']:
            continue
        start = 0
        while start < len(r['ops']):
            if not supported(r['ops'][start]):
                start += 1
                continue
            end = start+1
            while end < len(r['ops']) and supported(r['ops'][end]):
                end += 1
            if end-start >= 2:
                key = (r['core'], r['pc']+2*start, tuple(r['ops'][start:end]))
                eligible[key] = eligible.get(key, 0)+r['count']
            start = end
    candidates = [{'core': c, 'pc': pc, 'ops': list(ops), 'count': n,
                   'weighted_ops': len(ops)*n, 'region': region(pc)}
                  for (c, pc, ops), n in eligible.items()]
    candidates.sort(key=lambda c: c['weighted_ops'], reverse=True)
    runs.sort(key=lambda r: len(r['ops'])*r['count'], reverse=True)
    opcode_counts = Counter()
    pc_opcodes = {}
    for r in runs:
        for j, op in enumerate(r['ops']):
            opcode_counts[op] += r['count']
            pc_opcodes.setdefault((r['core'], r['pc']+2*j), set()).add(op)
    changed_pcs = [{'core':c, 'pc':pc, 'opcodes':sorted(ops)}
                   for (c, pc), ops in pc_opcodes.items() if len(ops)>1]
    # Exact byte matches show that a RAM run also exists in the ROM. They do
    # not establish the copy's origin or future stability; retain ambiguity.
    rom = (out/'input.32x').read_bytes()
    matches = []
    matched_dispatches = 0
    for r in runs:
        if r['region'] != 'sdram' or len(r['ops']) < 4 or r['delay']:
            continue
        blob = b''.join(op.to_bytes(2, 'big') for op in r['ops'])
        offset = rom.find(blob)
        if offset < 0:
            continue
        matched_dispatches += r['count'] * len(r['ops'])
        matches.append({'core': r['core'], 'ram_pc': r['pc'], 'ops': r['ops'],
                        'count': r['count'], 'first_rom_offset': offset,
                        'unique_rom_match': rom.find(blob, offset+1) < 0})
    (out/'rom-matches.json').write_text(json.dumps(matches, indent=2)+'\n')
    result = {'status': 'complete_guest_dispatch_census', 'cores': cores,
              'observed_dispatches': observed, 'dropped_dispatches': int(dropped[1]),
              'unique_dispatch_runs': len(runs), 'alu_dispatches': alu,
              'alu_dispatch_percent': 100*alu/observed if observed else 0,
              'multi_alu_run_dispatches': sum(c['weighted_ops'] for c in candidates),
              'top_alu_candidates': candidates[:30], 'top_dispatch_runs': runs[:30],
              'top_opcodes': [{'opcode': hex(op), 'count': n} for op, n in opcode_counts.most_common(30)],
              'sdram_run_rom_matches': len(matches),
              'sdram_matched_dispatches': matched_dispatches,
              'observed_code_changes': changed_pcs,
              'gate3_pass': 'GATE3 PASS' in log,
              'limitations': [
                  'Dynamic dispatch runs split at timeslices, PC discontinuities, delay slots, and 32 instructions; not canonical basic blocks.',
                  'Existing fastloop folds remain enabled; folded instructions are excluded from observed dispatch counts.',
                  'Frequency is not wall-time attribution or hardware speed.',
                  'QEMU icount disabled; all rig timing and instruction-cost output is invalid.',
                  'Game scene requires framebuffer inspection; no gameplay claim from checksums alone.',
                  'ALU subsequences count dispatched operations only; cycles, IRQ guards, lookup costs and code stability can reduce usable coverage.']}
    (out/'runs.json').write_text(json.dumps(runs, indent=2)+'\n')
    (out/'alu-candidates.json').write_text(json.dumps(candidates, indent=2)+'\n')
    (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    memory = analyze_memory(log,out)
    if memory:
        result['memory_result'] = str(out/'memory-result.json')
        (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--frames', type=int, default=720)
    parser.add_argument('--from-frame', type=int, default=540)
    parser.add_argument('--timeout', type=int, default=600)
    parser.add_argument('--start-frame', type=int, default=1320)
    parser.add_argument('--start-duration', type=int, default=120)
    parser.add_argument('--start-pattern', choices=['early-mash', 'single', 'rig'], default='early-mash')
    parser.add_argument('--memory-diagnostic', action='store_true')
    parser.add_argument('--performance-only', action='store_true', help='Disable observer; use existing M7 icount meter')
    parser.add_argument('--countdown-fold', action='store_true', help='Experimental generic RAM countdown loop fold')
    parser.add_argument('--pixel-native', action='store_true')
    parser.add_argument('--pixel-proof', action='store_true')
    parser.add_argument('--native-math', action='store_true')
    parser.add_argument('--math-proof', action='store_true')
    parser.add_argument('--control-native', action='store_true')
    parser.add_argument('--control-proof', action='store_true')
    parser.add_argument('--lookup-native', action='store_true')
    parser.add_argument('--lookup-proof', action='store_true')
    parser.add_argument('--pixel-tail', action='store_true')
    parser.add_argument('--chain-native', action='store_true')
    parser.add_argument('--chain-proof', action='store_true')
    parser.add_argument('--phase-profile', action='store_true')
    parser.add_argument('--inactive-fold', action='store_true')
    parser.add_argument('--pcm-tail-native', action='store_true')
    parser.add_argument('--pcm-tail-proof', action='store_true')
    parser.add_argument('--merged-dispatch', action='store_true')
    parser.add_argument('--auto-native', action='store_true')
    parser.add_argument('--core-source', type=Path)
    args = parser.parse_args()
    selection=None
    if args.auto_native:
        selection=native_selection.select(args.rom.read_bytes())
        for name,value in selection['enabled'].items():setattr(args,name,value)
        args.merged_dispatch=False
    if not 0 <= args.from_frame < args.frames or args.frames < 302:
        parser.error('Require 0 <= from-frame < frames and frames >= 302 for framebuffer gate')
    if args.math_proof and not args.native_math:
        parser.error('--math-proof requires --native-math')
    if args.pixel_proof and not args.pixel_native:
        parser.error('--pixel-proof requires --pixel-native')
    if args.pixel_native and not args.performance_only:
        parser.error('--pixel-native requires --performance-only')
    if args.control_proof and not args.control_native:
        parser.error('--control-proof requires --control-native')
    if args.control_native and not args.performance_only:
        parser.error('--control-native requires --performance-only')
    if args.lookup_proof and not args.lookup_native:
        parser.error('--lookup-proof requires --lookup-native')
    if args.lookup_native and not args.performance_only:
        parser.error('--lookup-native requires --performance-only')
    if args.pixel_tail and not args.pixel_native:
        parser.error('--pixel-tail requires --pixel-native')
    if args.chain_native and not args.control_native:
        parser.error('--chain-native requires --control-native')
    if args.chain_proof and not args.chain_native:
        parser.error('--chain-proof requires --chain-native')
    if args.inactive_fold and not args.chain_native:
        parser.error('--inactive-fold requires --chain-native')
    if args.pcm_tail_native and not args.control_native:
        parser.error('--pcm-tail-native requires --control-native')
    if args.pcm_tail_proof and not args.pcm_tail_native:
        parser.error('--pcm-tail-proof requires --pcm-tail-native')
    out = args.output.resolve()
    rel = out.relative_to(ROOT)
    out.mkdir(parents=True, exist_ok=False)
    if selection is not None:(out/'selection.json').write_text(json.dumps(selection,indent=2)+'\n')
    source = out/'picodrive'
    shutil.copytree(args.core_source if args.core_source else ROOT/'external/picodrive', source,
                    ignore=shutil.ignore_patterns('.git', '*.o', '*.a'))
    hook = (ROOT/'tools/32x_research/guest_trace.h').read_text()
    if args.memory_diagnostic:
        mem_hook = (ROOT/'tools/32x_research/memory_trace.h').read_text()
        hook = hook.replace('static void trace_tick(', mem_hook+'\nstatic void trace_tick(')
        hook = hook.replace('  unsigned c=s->is_slave&1;\n  if (rig_frame_no',
                            '  mem_pre(s,op);\n  unsigned c=s->is_slave&1;\n  if (rig_frame_no')
        hook = hook.replace('  printf("TRACE_END\\n");', '  mem_report();\n  printf("TRACE_END\\n");')
    path = source/'cpu/sh2/mame/sh2pico.c'
    text = path.read_text()
    anchor = 'int sh2_execute_interpreter(SH2 *sh2, int cycles)\n{'
    assert text.count(anchor) == 2
    text = text.replace(anchor, hook+'\n'+anchor, 1)
    text = text.replace('\tsh2->icount = cycles;', '\ttrace_flush(sh2->is_slave&1);\n\tsh2->icount = cycles;', 1)
    anchor = '\t\tRIG_SH2_TICK();'
    assert text.count(anchor) == 2
    text = text.replace(anchor, '\t\ttrace_tick(sh2, opcode, rig_is_delay);\n'+anchor, 1)
    text = text.replace('out:\n\treturn sh2->icount;', 'out:\n\ttrace_flush(sh2->is_slave&1);\n\treturn sh2->icount;', 1)
    if args.performance_only:
        text = text.replace('\t\ttrace_tick(sh2, opcode, rig_is_delay);', '\t\t(void)0; /* observer disabled for performance */')
    if args.countdown_fold:
        signature = 'int sh2_execute_interpreter(SH2 *sh2, int cycles)\n{'
        text = text.replace(signature, (ROOT/'tools/32x_research/countdown_fold.h').read_text()+'\n'+signature, 1)
        text = text.replace('\t\tRIG_OPCOST_T1(sh2, opcode);',
                            '\t\tif (opcode==0x8bfa && sh2->pc+8==sh2->ppc) countdown_fold(sh2);\n\t\tRIG_OPCOST_T1(sh2, opcode);', 1)
        text += '\nvoid countdown_report(void){printf("COUNTDOWN calls=%u iterations=%llu\\n", countdown_calls, countdown_iterations);}\n'
    if args.pixel_native:
        signature = 'int sh2_execute_interpreter(SH2 *sh2, int cycles)\n{'
        pixel = '#include <string.h>\n#include <stdlib.h>\n'+(ROOT/'tools/32x_research/pixel_native.h').read_text()
        text = text.replace(signature, pixel+'\n'+signature, 1)
        gate = '\t\tcase 0x6: op0110(sh2, opcode); break;'
        assert text.count(gate) == 1
        text = text.replace(gate, '\t\tcase 0x6: if (opcode==0x612c && !rig_is_delay && pixel_native(sh2,sh2->ppc)) { RIG_OPCOST_T1(sh2,opcode); continue; } op0110(sh2, opcode); break;', 1)
        text += '\nvoid pixel_report(void){printf("PIXEL calls=%u proofs=%u declines=%u groups=%u\\n",pixel_calls,pixel_proofs,pixel_declines,pixel_groups); }\n'
    if args.control_native:
        signature = 'int sh2_execute_interpreter(SH2 *sh2, int cycles)\n{'
        text = text.replace(signature,(ROOT/'tools/32x_research/control_native.h').read_text()+'\n'+signature,1)
        for major,op,kind in [(2,0x201f,0),(3,0x3d88,1)]:
            handler='op00'+format(major,'02b')
            gate=f'\t\tcase 0x{major:x}: {handler}(sh2, opcode); break;'
            assert text.count(gate)==1,gate
            text=text.replace(gate,f'\t\tcase 0x{major:x}: if(opcode==0x{op:04x} && !rig_is_delay && control_native(sh2,sh2->ppc,{kind})) {{ RIG_OPCOST_T1(sh2,opcode);continue; }} {handler}(sh2,opcode);break;',1)
        text += '\nvoid control_report(void){for(unsigned i=0;i<3;i++)printf("CONTROL kind=%u calls=%u proofs=%u taken=%u\\n",i,control_calls[i],control_proofs[i],control_taken[i]);}\n'
    if args.lookup_native:
        signature = 'int sh2_execute_interpreter(SH2 *sh2, int cycles)\n{'
        text=text.replace(signature,(ROOT/'tools/32x_research/lookup_native.h').read_text()+'\n'+signature,1)
        gate='\t\tcase 0x0: op0000(sh2, opcode); break;'
        assert text.count(gate)==1
        text=text.replace(gate,'\t\tcase 0x0: if(opcode==0x010d && !rig_is_delay && lookup_native(sh2,sh2->ppc)) { RIG_OPCOST_T1(sh2,opcode);continue; } op0000(sh2,opcode);break;',1)
        text += '\nvoid lookup_report(void){printf("LOOKUP calls=%u groups=%u proofs=%u\\n",lookup_calls,lookup_groups,lookup_proofs);}\n'
    if args.chain_native:
        signature = 'int sh2_execute_interpreter(SH2 *sh2, int cycles)\n{'
        text=text.replace(signature,(ROOT/'tools/32x_research/chain_native.h').read_text()+'\n'+signature,1)
        gate='\t\tcase 0x5: op0101(sh2, opcode); break;'
        assert text.count(gate)==1
        text=text.replace(gate,'\t\tcase 0x5: if(opcode==0x50e6 && !rig_is_delay && chain_native(sh2,sh2->ppc)) { RIG_OPCOST_T1(sh2,opcode);continue; } op0101(sh2,opcode);break;',1)
        text += '\nvoid chain_report(void){printf("CHAIN calls=%u parts=%u proofs=%u inactive_calls=%u inactive_iterations=%u\\n",chain_calls,chain_parts,chain_proofs,inactive_calls,inactive_iterations);}\n'
    if args.control_native:
        gate='\t\tcase 0x5:'
        # Match the interpreter arm, not optional skeleton diagnostics.
        suffix=' if(opcode==0x50e6' if args.chain_native else ' op0101(sh2, opcode); break;'
        assert text.count(gate+suffix)==1
        text=text.replace(gate+suffix,gate+' if(opcode==0x52e1 && !rig_is_delay && control_native(sh2,sh2->ppc,2)) { RIG_OPCOST_T1(sh2,opcode);continue; }'+suffix,1)
    if args.pcm_tail_native:
        signature = 'int sh2_execute_interpreter(SH2 *sh2, int cycles)\n{'
        text=text.replace(signature,(ROOT/'tools/32x_research/pcm_tail_native.h').read_text()+'\n'+signature,1)
        gate='\t\tcase 0xD: op1101(sh2, opcode); break;'
        assert text.count(gate)==1
        text=text.replace(gate,'\t\tcase 0xD: if(opcode==0xd21c && !rig_is_delay && pcm_tail_native(sh2,sh2->ppc)) { RIG_OPCOST_T1(sh2,opcode);continue; } op1101(sh2,opcode);break;',1)
        text += '\nvoid pcm_tail_report(void){printf("PCM_TAIL calls=%u proofs=%u\\n",pcm_tail_calls,pcm_tail_proofs);}\n'
    if args.memory_diagnostic:
        text = text.replace('\t\tsh2->icount--;', '\t\tmem_post(sh2);\n\t\tsh2->icount--;', 1)
    if args.native_math:
        math_path=source/'cpu/sh2/mame/sh2.c'
        math_path.write_text(native_math.inject(math_path.read_text()))
        text += native_math.PROOF
    if args.merged_dispatch:
        dispatch_path=source/'cpu/sh2/mame/sh2.c'
        text,dispatch=merged_dispatch.inject(text,dispatch_path.read_text(),args)
        dispatch_path.write_text(dispatch)
    path.write_text(text)
    rig = out/'rig'
    shutil.copytree(ROOT/'tools/m7_qemu_rig', rig,
                    ignore=shutil.ignore_patterns('build', '*.o', '*.elf'))
    path = rig/'rig_32x.c'
    text = path.read_text()
    # Define the existing PC-hist flag for its delay tracking variable, but
    # remove its per-instruction census and reporter: one instrument per run.
    text = text.replace('    rig_pchist_report();', '    trace_report();')
    text = text.replace('int rig_frame_no;', 'int rig_frame_no;\nextern void trace_report(void);')
    if args.countdown_fold:
        text = text.replace('extern void trace_report(void);','extern void trace_report(void);\nextern void countdown_report(void);')
        text = text.replace('    trace_report();','    trace_report();\n    countdown_report();')
    if args.pixel_native:
        text = text.replace('extern void trace_report(void);','extern void trace_report(void);\nextern void pixel_report(void);')
        text = text.replace('    trace_report();','    trace_report();\n    pixel_report();')
    if args.control_native:
        text = text.replace('extern void trace_report(void);','extern void trace_report(void);\nextern void control_report(void);')
        text = text.replace('    trace_report();','    trace_report();\n    control_report();')
    if args.lookup_native:
        text = text.replace('extern void trace_report(void);','extern void trace_report(void);\nextern void lookup_report(void);')
        text = text.replace('    trace_report();','    trace_report();\n    lookup_report();')
    if args.control_proof:
        text=text.replace('extern void trace_report(void);','extern void trace_report(void);\nextern void control_synthetic_proof(SH2 *base);')
        text=text.replace('    for (int f = 0; f < RIG_FRAMES; f++) {','    control_synthetic_proof(sh2s);\n    for (int f = 0; f < RIG_FRAMES; f++) {')
    if args.chain_native:
        text = text.replace('extern void trace_report(void);','extern void trace_report(void);\nextern void chain_report(void);')
        text = text.replace('    trace_report();','    trace_report();\n    chain_report();')
    if args.chain_proof:
        text=text.replace('extern void trace_report(void);','extern void trace_report(void);\nextern void chain_synthetic_proof(SH2 *base);')
        text=text.replace('    for (int f = 0; f < RIG_FRAMES; f++) {','    chain_synthetic_proof(sh2s);\n    for (int f = 0; f < RIG_FRAMES; f++) {')
    if args.pcm_tail_native:
        text = text.replace('extern void trace_report(void);','extern void trace_report(void);\nextern void pcm_tail_report(void);')
        text = text.replace('    trace_report();','    trace_report();\n    pcm_tail_report();')
    if args.pcm_tail_proof:
        text=text.replace('extern void trace_report(void);','extern void trace_report(void);\nextern void pcm_tail_synthetic_proof(SH2 *base);')
        text=text.replace('    for (int f = 0; f < RIG_FRAMES; f++) {','    pcm_tail_synthetic_proof(sh2s);\n    for (int f = 0; f < RIG_FRAMES; f++) {')
    if args.math_proof:
        text = text.replace('extern void trace_report(void);','extern void trace_report(void);\nextern void native_math_proof(SH2 *base);')
        text = text.replace('    for (int f = 0; f < RIG_FRAMES; f++) {','    native_math_proof(sh2s);\n    for (int f = 0; f < RIG_FRAMES; f++) {')
    anchor = '        PicoIn.pad[0] = pad_script(f);\n        uint32_t t0'
    assert text.count(anchor) == 1
    start_end = args.start_frame+args.start_duration
    pad_expr = '(f < 120 && f % 12 < 6)' if args.start_pattern == 'early-mash' else f'(f >= {args.start_frame} && f < {start_end})'
    pad_value='pad_script(f)' if args.start_pattern=='rig' else pad_expr+' ? PAD_START : 0'
    text = text.replace(anchor, f'        PicoIn.pad[0] = {pad_value};\n        uint32_t t0')
    text = text.replace('if (f == RIG_FB_DUMP) fb_dump_rle(f);',
                        'if (f == TRACE_FROM-1 || f == RIG_FRAMES-1) fb_dump_rle(f);')
    path.write_text(text)
    path = source/'cpu/sh2/mame/sh2pico.c'
    text = path.read_text().replace('RIG_PC_HIST_TICK(sh2, rig_is_delay, (unsigned short)opcode);', '(void)0; /* replaced by exact dispatch-run census */')
    path.write_text(text)
    script = (ROOT/'tools/m7_qemu_rig/run_32x.sh').read_text()
    script = script.replace('cd "$(dirname "$0")/../.."', 'cd /opt/workdir')
    script = script.replace('RIG=tools/m7_qemu_rig', 'RIG='+str(rel/'rig'))
    # GCC 15 promoted this existing bus API typedef warning to an error;
    # both types are 32-bit on M7. Preserve source and expose the warning.
    script = script.replace('OPT="-O2 ', 'OPT="-O2 -Wno-error=incompatible-pointer-types ')
    (out/'build.sh').write_text(script)
    # Resolve external inputs by copying to this disk-backed evidence directory.
    shutil.copyfile(args.rom, out/'input.32x')
    manifest = {'rom_sha256': hashlib.sha256((out/'input.32x').read_bytes()).hexdigest(),
                'rom_input': str(args.rom.resolve()), 'frames': args.frames,
                'window': [args.from_frame, args.frames], 'pad': 'existing rig pad_script(f)' if args.start_pattern=='rig' else pad_expr+' ? START : 0',
                'icount': False, 'device_access': False, 'source_hashes': {}}
    manifest['memory_diagnostic'] = args.memory_diagnostic
    manifest['performance_only'] = args.performance_only
    manifest['countdown_fold'] = args.countdown_fold
    manifest['pixel_native'] = args.pixel_native
    manifest['pixel_proof'] = args.pixel_proof
    manifest['native_math'] = args.native_math
    manifest['math_proof'] = args.math_proof
    manifest['control_native']=args.control_native
    manifest['control_proof']=args.control_proof
    manifest['lookup_native']=args.lookup_native
    manifest['lookup_proof']=args.lookup_proof
    manifest['pixel_tail']=args.pixel_tail
    manifest['chain_native']=args.chain_native
    manifest['chain_proof']=args.chain_proof
    manifest['phase_profile']=args.phase_profile
    manifest['inactive_fold']=args.inactive_fold
    manifest['pcm_tail_native']=args.pcm_tail_native
    manifest['pcm_tail_proof']=args.pcm_tail_proof
    manifest['merged_dispatch']=args.merged_dispatch
    manifest['auto_native']=args.auto_native
    manifest['selection']=selection
    manifest['core_source']=str(args.core_source.resolve()) if args.core_source else str(ROOT/'external/picodrive')
    manifest['icount'] = args.performance_only
    for p in source.rglob('*.c'):
        manifest['source_hashes'][str(p.relative_to(source))] = hashlib.sha256(p.read_bytes()).hexdigest()
    (out/'inputs.json').write_text(json.dumps(manifest, indent=2)+'\n')
    defs = f'-DRIG_SH2_PC_HIST -DRIG_FB_DUMP=1 -DTRACE_FROM={args.from_frame} -DTRACE_TO={args.frames}'
    if args.start_pattern=='rig':
        defs += ' -DRIG_PAD_SCRIPT'
    if args.performance_only:
        defs += f' -DRIG_WARMUP={args.from_frame}'
    if args.pixel_proof:
        defs += ' -DPIXEL_SHADOW_PROOF'
    if args.math_proof:
        defs += ' -DNATIVE_MATH_PROOF'
    if args.control_proof:
        defs += ' -DCONTROL_SHADOW_PROOF'
    if args.lookup_proof:
        defs += ' -DLOOKUP_SHADOW_PROOF'
    if args.pixel_tail:
        defs += ' -DPIXEL_NATIVE_TAIL'
    if args.chain_proof:
        defs += ' -DCHAIN_SHADOW_PROOF'
    if args.phase_profile:
        defs += ' -DRIG_PHASE_PROF'
    if args.inactive_fold:
        defs += ' -DNATIVE_INACTIVE_FOLD'
    if args.pcm_tail_proof:
        defs += ' -DPCM_TAIL_SHADOW_PROOF'
    command = ['docker', 'run', '--rm', '--init', '--user', '1000:1000',
               '-v', str(ROOT)+':/opt/workdir', '-e', 'PATH=/opt/arm-gnu-toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
               '-e', 'PD_DIR='+str(rel/'picodrive'), '-e', 'RIG_OUT='+str(rel/'objects'),
               '-e', 'RIG_BUILD_ONLY=1', '-e', 'EXTRA_DEF='+defs,
               'sylverb/retro-go-sd-builder:v1.5', 'bash', str(rel/'build.sh'), str(rel/'input.32x'), str(args.frames)]
    started = time.monotonic()
    try:
        with (out/'build.log').open('w') as log:
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
        with (out/'qemu.log').open('w') as log:
            qemu = ['qemu-system-arm', '-machine', 'mps2-an500', '-nographic', '-semihosting']
            if args.performance_only:
                qemu += ['-icount','shift=0,align=off,sleep=off']
            subprocess.run(qemu+['-kernel', str(out/'objects/rig_32x.elf')], stdout=log,
                           stderr=subprocess.STDOUT, check=True, timeout=args.timeout)
        raw = (out/'qemu.log').read_text()
        result = analyze(raw, out)
        if args.performance_only:
            meter = re.search(r'done (\d+) frames  avg host=(\d+)  min=(\d+)  max=(\d+) insn/frame  avg sh2=(\d+)',raw)
            if not meter or 'GATE3 PASS' not in raw:
                raise ValueError('Incomplete full-game M7 icount benchmark')
            result['status']='full_game_m7_icount_complete'
            result['meter']=dict(zip(['frames','host_insns_per_frame','min','max','sh2_dispatches_per_frame'],map(int,meter.groups())))
            result['window']=[args.from_frame,args.frames]
            result['signatures']=[l for l in raw.splitlines() if any(k in l for k in ('snd hash=','snd energy','fb f'))]
            result['limitations']=['M7 icount cost for full-game window; physical cache/XIP latency excluded. Not device FPS.', 'Sparse frame signatures and complete sound checksum; not full per-frame state equality.']
            match=re.search(r'COUNTDOWN calls=(\d+) iterations=(\d+)',raw)
            if match:
                result['fold']=dict(zip(['calls','iterations'],map(int,match.groups())))
            match=re.search(r'PIXEL calls=(\d+) proofs=(\d+) declines=(\d+) groups=(\d+)',raw)
            if match:
                result['pixel']=dict(zip(['calls','proofs','declines','groups'],map(int,match.groups())))
            match=re.search(r'MATH_PROOF checks=(\d+) context=(\d+)',raw)
            if match:
                result['math_proof']=dict(zip(['checks','context_bytes'],map(int,match.groups())))
            result['control']=[dict(zip(['kind','calls','proofs','taken'],map(int,v))) for v in re.findall(r'CONTROL kind=(\d+) calls=(\d+) proofs=(\d+) taken=(\d+)',raw)]
            match=re.search(r'LOOKUP calls=(\d+) groups=(\d+) proofs=(\d+)',raw)
            if match:
                result['lookup']=dict(zip(['calls','groups','proofs'],map(int,match.groups())))
            match=re.search(r'CONTROL_SYNTH checks=(\d+) declines=(\d+) mul_t0=(\d+) mul_t1=(\d+) coord_t0=(\d+) coord_t1=(\d+) interp_t0=(\d+) interp_t1=(\d+)',raw)
            if match:
                result['control_synthetic']=dict(zip(['checks','declines','mul_t0','mul_t1','coord_t0','coord_t1','interp_t0','interp_t1'],map(int,match.groups())))
            match=re.search(r'CHAIN calls=(\d+) parts=(\d+) proofs=(\d+)',raw)
            if match:
                result['chain']=dict(zip(['calls','parts','proofs'],map(int,match.groups())))
            if args.phase_profile:
                rows=re.findall(r'^\[32x-phase\]\s+(.+?)\s+(\d+)\s+(\d+\.\d+)%',raw,re.M)
                if not rows or '[32x-phase] WARN' in raw:
                    raise ValueError('Incomplete or leaking phase profiler')
                result['status']='full_game_m7_icount_profile_complete'
                result['phase_rows']=[{'phase':name,'host_insns_per_frame':int(cost),'percent':float(percent)} for name,cost,percent in rows]
                result['limitations'].append('Phase probes enabled: diagnostic attribution only; meter cost is not the uninstrumented candidate benchmark.')
            match=re.search(r'inactive_calls=(\d+) inactive_iterations=(\d+)',raw)
            if match:
                result['inactive']=dict(zip(['calls','iterations'],map(int,match.groups())))
            match=re.search(r'CHAIN_SYNTH checks=(\d+) declines=(\d+)',raw)
            if match:
                result['chain_synthetic']=dict(zip(['checks','declines'],map(int,match.groups())))
            match=re.search(r'PCM_TAIL calls=(\d+) proofs=(\d+)',raw)
            if match:
                result['pcm_tail']=dict(zip(['calls','proofs'],map(int,match.groups())))
            match=re.search(r'PCM_TAIL_SYNTH checks=(\d+) declines=(\d+)',raw)
            if match:
                result['pcm_tail_synthetic']=dict(zip(['checks','declines'],map(int,match.groups())))
            (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        starts = [m.start() for m in re.finditer(r'^\[fbd\].*w=', raw, re.M)]
        for i, start in enumerate(starts):
            stop = starts[i+1] if i+1 < len(starts) else len(raw)
            frame = args.from_frame-1 if i == 0 else args.frames-1
            screen_log = out/f'screen-{frame}.log'
            screen_log.write_text(raw[start:stop])
            pixels = []
            for count, value in re.findall(r'^\[fbd\] (\d+) ([0-9a-f]{4})\s*$', raw[start:stop], re.M):
                pixels.extend([int(value,16)]*int(count))
            if len(pixels) != 320*240:
                raise ValueError('Incomplete framebuffer RLE')
            screen_raw = out/f'screen-{frame}.raw'
            screen_raw.write_bytes(struct.pack('<76800H', *pixels))
            # The rig selects PDF_RGB555. Use the existing RGB555 decoder;
            # its legacy fbdump utility assumes RGB565 and changes colours.
            subprocess.run([sys.executable, str(ROOT/'tools/pico_host/raw2png.py'),
                            str(screen_raw), str(out/f'screen-{frame}.png')], check=True, timeout=30)
    except (subprocess.SubprocessError, ValueError) as exc:
        (out/'failure.json').write_text(json.dumps({'status':'failed_or_incomplete', 'reason':str(exc)}, indent=2)+'\n')
        raise
    brief = {'status':result['status'], 'elapsed_seconds':round(time.monotonic()-started,1),
             'result':str(out/'result.json')}
    if args.performance_only:
        brief['meter'] = result['meter']
        if 'fold' in result:
            brief['fold'] = result['fold']
    else:
        brief.update(dispatches=result['observed_dispatches'],runs=result['unique_dispatch_runs'])
    print(json.dumps(brief))


if __name__ == '__main__':
    main()
