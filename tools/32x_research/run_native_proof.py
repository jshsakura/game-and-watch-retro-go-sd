#!/usr/bin/env python3
"""Finite offline generate/build/QEMU proof. Never access hardware or poll jobs."""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--rom', action='append', default=[])
parser.add_argument('--trace-candidates', type=Path)
args = parser.parse_args()
out = args.output.resolve()
generate = [sys.executable, str(root/'tools/32x_research/native_blocks.py'), '--output', str(out)]
for rom in args.rom:
    generate += ['--rom', rom]
if args.trace_candidates:
    generate += ['--trace-candidates', str(args.trace_candidates.resolve())]
subprocess.run(generate, check=True, timeout=30)
relative = out.relative_to(root)
source_manifest = {}
for name in ['tools/32x_research/native_blocks.py', 'tools/32x_research/native_proof.c',
             'tools/32x_research/native_reference.c', 'tools/32x_research/run_native_proof.py',
             'tools/m7_qemu_rig/rig_runtime.c', 'tools/m7_qemu_rig/mps2_an500.ld',
             'external/picodrive/cpu/sh2/mame/sh2pico.c',
             'external/picodrive/cpu/sh2/mame/sh2.c', 'external/picodrive/cpu/sh2/sh2.h',
             'external/picodrive/pico/pico_types.h', 'external/picodrive/pico/pico_port.h']:
    target = out/'source'/name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root/name, target)
    source_manifest[name] = hashlib.sha256(target.read_bytes()).hexdigest()
(out/'source-manifest.json').write_text(json.dumps(source_manifest, indent=2)+'\n')
container = ['docker', 'run', '--rm', '--init', '--user', '1000:1000',
             '-v', str(root)+':/opt/workdir', 'sylverb/retro-go-sd-builder:v1.5']
arch = ['-mcpu=cortex-m7', '-mthumb', '-mfloat-abi=hard', '-mfpu=fpv5-d16', '-ffp-contract=off']
definitions = ['-DGNW_32X_CORE', '-DEMU_G68K', '-DTABLES_FULL', '-D_USE_CZ80']
command = container + ['/opt/arm-gnu-toolchain/bin/arm-none-eabi-gcc', *arch,
    '-O2', '-g', '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections',
    *definitions, '-Iexternal/picodrive', '-I'+str(relative),
    'tools/32x_research/native_reference.c', 'tools/32x_research/native_proof.c',
    'tools/m7_qemu_rig/rig_runtime.c', '-Ttools/m7_qemu_rig/mps2_an500.ld',
    '-nostartfiles', '-Wl,--gc-sections', '-Wl,-Map='+str(relative/'proof.map'),
    '-lc', '-lm', '-o', str(relative/'proof.elf')]
with (out/'build.log').open('w') as log:
    subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
for filename, flags in [('disassembly.txt', ['-d']), ('symbols.txt', ['-t'])]:
    with (out/filename).open('w') as log:
        subprocess.run(container+['/opt/arm-gnu-toolchain/bin/arm-none-eabi-objdump', *flags,
                                  str(relative/'proof.elf')], cwd=root, stdout=log, check=True, timeout=30)
with (out/'qemu.log').open('w') as log:
    subprocess.run(['qemu-system-arm', '-machine', 'mps2-an500', '-nographic', '-semihosting',
        '-icount', 'shift=0,align=off,sleep=off', '-kernel', str(out/'proof.elf')],
        stdout=log, stderr=subprocess.STDOUT, check=True, timeout=240)
log = (out/'qemu.log').read_text()
if not re.search(r'^PASS register-only', log, re.M) or re.search(r'^FAIL', log, re.M):
    raise ValueError('QEMU did not report a valid completed proof')
proof = re.search(r'PROOF blocks=(\d+) state_comparisons=(\d+) unchanged_declines=(\d+) context_bytes=(\d+)', log)
benches = [dict(zip(['block', 'guest_ops', 'repeats', 'baseline_ticks', 'native_ticks'], map(int, v)))
           for v in re.findall(r'BENCH block=(\d+) guest_ops=(\d+) repeats=(\d+) baseline_ticks=(\d+) native_ticks=(\d+)', log)]
for b in benches:
    b['instruction_cost_reduction_percent'] = (1-b['native_ticks']/b['baseline_ticks'])*100
    b['source'] = json.loads((out/'inputs.json').read_text())['blocks'][b['block']]
symbols = (out/'symbols.txt').read_text()
native_sizes = [int(v,16) for v in re.findall(r'^\S+\s+\w+\s+F\s+\.text\s+([0-9a-f]+)\s+native_\d+\s*$', symbols, re.M)]
disassembly = (out/'disassembly.txt').read_text()
calls = []
current = None
for line in disassembly.splitlines():
    start = re.match(r'^[0-9a-f]+ <([^>]+)>:', line)
    if start:
        current = start[1] if re.fullmatch(r'native_\d+', start[1]) else None
    if current and re.search(r'\sblx?(?:\.w)?\s', line):
        calls.append({'function':current, 'instruction':line.strip()})
audit = {'native_functions':len(native_sizes), 'calls':calls}
(out/'code-audit.json').write_text(json.dumps(audit, indent=2)+'\n')
if calls:
    raise ValueError('Generated native ALU functions contain calls; inspect code-audit.json')
result = {'status': 'offline_native_alu_proof_passed',
          'proof': dict(zip(['blocks','state_comparisons','unchanged_declines','context_bytes'], map(int, proof.groups()))),
          'benchmarks': benches, 'generated_function_text_bytes': sum(native_sizes),
          'native_function_calls':len(calls),
          'elf_sha256': hashlib.sha256((out/'proof.elf').read_bytes()).hexdigest(),
          'build_command': command, 'limitations': [
              'One-cycle register-only instructions; no memory, branches, actual IRQ delivery or fallback integration.',
              'All comparisons against current interpreter, with unexpected buses/IRQ trapped.',
              'Static ROM sample reachability is unverified. Optional traced ALU runs were dispatched in their recorded window, but proof uses relocated code and synthetic register states.',
              'QEMU instruction-cost samples omit flash latency/cache stalls; not device FPS.',
              'Standalone native functions; no deployable sidecar loader, index or invalidation yet.']}
(out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps({'status': result['status'], 'proof': result['proof'],
                  'generated_function_text_bytes': result['generated_function_text_bytes'], 'result': str(out/'result.json')}))
