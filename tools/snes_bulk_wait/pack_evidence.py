#!/usr/bin/env python3
"""Preserve completed evidence and failed cases; never run or retry a guest."""
import argparse
import array
import gzip
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys

ap = argparse.ArgumentParser()
ap.add_argument('--build', type=Path, required=True)
ap.add_argument('--out', type=Path, required=True)
args = ap.parse_args()
root = Path(__file__).resolve().parents[2]
args.out.mkdir(parents=True, exist_ok=False)
sha = lambda data: hashlib.sha256(data).hexdigest()
artifacts = {}


def save(name, data, compressed=False):
    p = args.out / name
    p.parent.mkdir(parents=True, exist_ok=True)
    content = gzip.compress(data, mtime=0) if compressed else data
    p.write_bytes(content)
    artifacts[name] = dict(sha256=sha(content), bytes=len(content), raw_sha256=sha(data))


runs, arms, cases, drift = {}, {}, {}, {}
for p in sorted(args.build.iterdir()):
    if not p.is_dir():
        continue
    if (p / 'build.json').exists():
        meta = json.loads((p / 'build.json').read_text())
        elf = (p / 'rig_snes.elf').read_bytes()
        if sha(elf) != meta['elf_sha256']:
            raise SystemExit(f'changed ELF: {p}')
        arms[p.name] = meta
        for name in ('build.json', 'build.log', 'rig_snes.elf'):
            compressed = name != 'build.json'
            save(f'arms/{p.name}/{name}' + ('.gz' if compressed else ''), (p / name).read_bytes(), compressed)
    elif (p / 'result.json').exists():
        result = json.loads((p / 'result.json').read_text())
        request = json.loads((p / 'request.json').read_text())
        if sha(Path(request['rom']).read_bytes()) != request['rom_sha256']:
            raise SystemExit(f'changed ROM: {p}')
        if sha(Path(request['elf']).read_bytes()) != request['elf_sha256']:
            raise SystemExit(f'changed ELF: {p}')
        row = dict(request=request, result=result)
        if (p / 'audio.pcm').exists():
            pcm = (p / 'audio.pcm').read_bytes()
            if sha(pcm) != result['pcm']['sha256']:
                raise SystemExit(f'changed PCM: {p}')
            samples = array.array('h', pcm)
            if sys.byteorder != 'little':
                samples.byteswap()
            row['pcm_activity'] = dict(samples=len(samples), nonzero=sum(s != 0 for s in samples),
                                       peak=max(map(abs, samples), default=0),
                                       rms=round(math.sqrt(sum(s * s for s in samples) / len(samples)), 4) if samples else 0,
                                       scope='raw DSP PCM before stretcher; audio bytes kept locally, not in git')
        runs[p.name] = row
        for name in ('request.json', 'result.json', 'output.log'):
            save(f'runs/{p.name}/{name}' + ('.gz' if name.endswith('.log') else ''), (p / name).read_bytes(), name.endswith('.log'))
for p in args.build.glob('*-summary.json'):
    value = json.loads(p.read_text())
    if value['correctness'] != 'PASS':
        raise SystemExit(f'invalid comparison: {p}')
    cases[p.stem.removesuffix('-summary')] = value
    save(p.name, p.read_bytes())
for p in args.build.glob('*-drift.json'):
    drift[p.stem.removesuffix('-drift')] = json.loads(p.read_text())
    save(p.name, p.read_bytes())
for name in ('audio-simulation.json', 'audio-stretch-test.log', 'evidence-tests.log', 'size.log', 'span-size.log', 'compiler.txt', 'host-compiler.txt', 'qemu.txt', 'container-image.txt'):
    save(name + ('.gz' if name.endswith('.log') else ''), (args.build / name).read_bytes(), name.endswith('.log'))
save('harness.patch.gz', subprocess.check_output(['git', 'diff', '--cached', '--binary'], cwd=root), True)
save('core.patch.gz', subprocess.check_output(['git', '-C', 'external/sm', 'diff', '5b77015292343a11591e7244c2a926b24c41b090', 'HEAD'], cwd=root), True)
core_dirty = subprocess.check_output(['git', '-C', 'external/sm', 'status', '--porcelain'], cwd=root, text=True).strip()
if core_dirty:
    raise SystemExit('unexpected dirty core')
manifest = dict(schema=1, date='2026-10-01', worktree=str(root),
                source_parent=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
                core_candidate=subprocess.check_output(['git', '-C', 'external/sm', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
                source_patch='harness.patch.gz', core_patch='core.patch.gz',
                compiler=(args.build / 'compiler.txt').read_text().strip(),
                host_compiler=(args.build / 'host-compiler.txt').read_text().strip(),
                container_id=(args.build / 'container-image.txt').read_text().strip(),
                qemu=(args.build / 'qemu.txt').read_text().strip(), arms=arms, runs=runs, cases=cases, drift=drift,
                guest_frames_completed=sum(r['result']['frames'] for r in runs.values() if r['result']['completed']),
                completed_runs=sum(r['result']['completed'] for r in runs.values()),
                failed_runs=[name for name, r in runs.items() if not r['result']['completed']],
                samples='one A and one B per workload; Zelda and SMW have one intentional A-after each; descriptive deterministic instruction counts',
                hardware=dict(run=False, ownership='assigned to 32X by user; wait for user to finish',
                              scene_52fps_save='not acquired', firmware_link='not built', completion_route='not verified'),
                accuracy='final actual save stream + every frame framebuffer/audio hash + SHA256 of all raw PCM; differential span gate retained in 20261001-span-fold',
                limits=['cold boots and scripted taps; gameplay identity with the 52.65 drawn FPS scene unverified',
                        'no QEMU caches, wait states, interrupts from hardware LCD/audio DMA or physical display transfer',
                        'device-video models direct drawing and framebuffer memcpy only',
                        'FF5 1200-frame timeout retained; 600-frame comparison is separate and its PCM is silent',
                        'PCM/WAV and sample ELF copies are local; compressed ROM-free ELFs retained here',
                        '60 drawn FPS and device firmware link/ITCM headroom remain unmeasured'],
                source_sha256={str(p.relative_to(root)): sha(p.read_bytes()) for p in [root / 'tools/m7_qemu_rig/rig_snes.c', *sorted((root / 'tools/snes_bulk_wait').glob('*.py')), root / 'tests/test_snes_bulk_evidence.py', root / 'tools/snes_stretch_sim/sim.c', root / 'Core/Src/porting/snes/snes_audio_stretch.c', root / 'Core/Src/porting/snes/snes_audio_stretch.h', root / 'tests/test_snes_audio_stretch.c']},
                artifacts=artifacts)
(args.out / 'manifest.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n')
print(json.dumps(dict(cases=len(cases), runs=len(runs), frames=manifest['guest_frames_completed'], failed=manifest['failed_runs'], destination=str(args.out))))
