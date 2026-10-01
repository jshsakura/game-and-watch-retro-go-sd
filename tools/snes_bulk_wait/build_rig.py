#!/usr/bin/env python3
"""Build one immutable harness arm, without device access or status polling."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ap = argparse.ArgumentParser()
ap.add_argument('--out', type=Path, required=True)
ap.add_argument('--bulk', action='store_true')
ap.add_argument('--device-video', action='store_true')
ap.add_argument('--frames', type=int, default=1200)
args = ap.parse_args()
if args.frames < 200 or args.frames % 200:
    ap.error('frames must be a positive multiple of the 200-frame window')
root = Path(__file__).resolve().parents[2]
out = args.out.resolve()
out.relative_to(root)
out.mkdir(parents=True, exist_ok=False)
# Loader mode leaves this empty blob unused; no copyrighted ROM in the ELF.
dummy = out / 'empty-rom.smc'
dummy.write_bytes(b'\0')
defines = '-DSNES_SPIN_BAKE -DSNES_PPU_VIRGIN_Z=1 -DSNES_PPU_BLEND_LUT=1 -DSNES_PPU_OPAQUE_TILE=1 -DSNES_SKIP_SPRITE_EVAL_ON_SKIP=1 -DRIG_ROM_LOADER -DRIG_CASE_CONFIG -DRIG_FRAME_DIST -DRIG_FRAME_TRACE -DRIG_STATE_GATE -DRIG_AUDIO_DUMP -DRIG_AUDIO_PATH=\"audio.pcm\"'
if args.bulk:
    defines += ' -DSNES_BAKE_BULK=1'
if args.device_video:
    defines += ' -DRIG_DEVICE_VIDEO -DRIG_DIRECT_VIDEO'
argv = ['docker', 'run', '--rm', '--init', '--network', 'none', '--user',
        f'{subprocess.check_output(["id", "-u"], text=True).strip()}:{subprocess.check_output(["id", "-g"], text=True).strip()}',
        '-v', f'{root}:/opt/workdir', '-e', 'RIG_BUILD_ONLY=1', '-e', f'RIG_OUT={out.relative_to(root)}',
        '-e', f'RIG_EXTRA_DEF={defines}', 'sylverb/retro-go-sd-builder:v1.5',
        'bash', 'tools/m7_qemu_rig/run_snes_t2.sh', str(dummy.relative_to(root)), str(args.frames)]
with (out / 'build.log').open('w') as log:
    completed = subprocess.run(argv, cwd=root, stdout=log, stderr=subprocess.STDOUT, timeout=60)
if completed.returncode:
    raise SystemExit((out / 'build.log').read_text()[-4000:])
elf = out / 'rig_snes.elf'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
sources = ['tools/m7_qemu_rig/rig_snes.c', 'tools/m7_qemu_rig/run_snes_t2.sh', 'external/sm/src/snes/spin_bake.c', 'external/sm/src/snes/spin_bake.h']
meta = dict(argv=argv, extra_defines=defines, bulk=args.bulk, device_video=args.device_video,
            frames=args.frames, window=200, elf_sha256=sha(elf), elf_bytes=elf.stat().st_size,
            parent_commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
            core_commit=subprocess.check_output(['git', '-C', 'external/sm', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
            source_sha256={p: sha(root / p) for p in sources},
            rom='runtime loader; one-byte dummy section; no ROM linked',
            performance='timer captures exclude PCM file I/O, hashes, trace printing and final save serialization')
(out / 'build.json').write_text(json.dumps(meta, indent=2) + '\n')
for p in (out / 'rom.smc', out / 'rom.o', dummy):
    p.unlink()
print(json.dumps(dict(elf=str(elf), elf_sha256=meta['elf_sha256'], bulk=args.bulk, device_video=args.device_video)))
