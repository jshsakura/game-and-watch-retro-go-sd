#!/usr/bin/env python3
"""One finite M7 case, foreground process completion, no status polling."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import time

ap = argparse.ArgumentParser()
ap.add_argument('--elf', type=Path, required=True)
ap.add_argument('--rom', type=Path, required=True)
ap.add_argument('--out', type=Path, required=True)
ap.add_argument('--timeout', type=float, default=25)
ap.add_argument('--frames', type=int, default=1200)
ap.add_argument('--case-config', action='store_true')
ap.add_argument('--force-gate', action='store_true')
ap.add_argument('--input', choices=['none', 'tap-v1'], default='none')
ap.add_argument('--audio-dump', action='store_true')
ap.add_argument('--rerun-reason')
args = ap.parse_args()
if (args.force_gate or args.input != 'none') and not args.case_config:
    ap.error('runtime flags require --case-config and a RIG_CASE_CONFIG ELF')
args.out.mkdir(parents=True, exist_ok=False)
elf, rom = args.elf.resolve(), args.rom.resolve()
fingerprint = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
request = dict(elf=str(elf), elf_sha256=fingerprint(elf), rom=str(rom),
               rom_sha256=fingerprint(rom), save=None, input=args.input,
               frames=args.frames, gate='forced' if args.force_gate else 'natural',
               workload='cold boot; no warmup', case_config=args.case_config,
               audio_dump=args.audio_dump,
               rerun_reason=args.rerun_reason,
               profiling='QEMU icount; no device cycles or FPS', timeout=args.timeout)
(args.out / 'request.json').write_text(json.dumps(request, indent=2) + '\n')
# QEMU loader filenames cannot contain commas. The symlink avoids copying ROMs.
link = (args.out / 'input.sfc').absolute()
link.symlink_to(rom)
length = (args.out / 'length.bin').absolute()
length.write_bytes(struct.pack('<I', rom.stat().st_size))
argv = ['qemu-system-arm', '-machine', 'mps2-an500', '-nographic', '-semihosting',
        '-icount', 'shift=0,align=off,sleep=off', '-kernel', str(elf),
        '-device', f'loader,file={link},addr=0x60800000,force-raw=on',
        '-device', f'loader,file={length},addr=0x607ffffc,force-raw=on']
flags = int(args.force_gate) | (2 if args.input == 'tap-v1' else 0)
if args.case_config:
    config = (args.out / 'config.bin').absolute()
    config.write_bytes(struct.pack('<II', 0x534e3630, flags))
    argv += ['-device', f'loader,file={config},addr=0x607fff00,force-raw=on']
request['argv'] = argv
(args.out / 'request.json').write_text(json.dumps(request, indent=2) + '\n')
started = time.monotonic()
with (args.out / 'output.log').open('w') as output:
    try:
        result = subprocess.run(argv, cwd=args.out.resolve(), stdout=output, stderr=subprocess.STDOUT,
                                timeout=args.timeout, check=False)
        code = result.returncode
    except subprocess.TimeoutExpired:
        code = 124
elapsed = time.monotonic() - started
log = (args.out / 'output.log').read_text()
done = re.search(r'done (\d+) frames STATEHASH=(\w+) AUDIOHASH=(\w+) '
                 r'avg emu=(\d+) apu=(\d+) insn/frame', log)
result = dict(exit_code=code, elapsed_seconds=round(elapsed, 3),
              completed=bool(code == 0 and done and int(done[1]) == args.frames), input_unchanged=fingerprint(rom) == request['rom_sha256'],
              binary_unchanged=fingerprint(elf) == request['elf_sha256'])
if done:
    result.update(frames=int(done[1]), statehash=done[2], audiohash=done[3],
                  emu_insn_per_frame=int(done[4]), apu_insn_per_frame=int(done[5]))
else:
    result['failure_tail'] = log.splitlines()[-6:]
result['case_verified'] = not args.case_config or f'[case] flags={flags} input={args.input} gate={request["gate"]}' in log
state = re.search(r'\[state\] bytes=(\d+) hash=([0-9a-f]{16})', log)
if state:
    result['serialized_state'] = dict(bytes=int(state[1]), hash=state[2])
if args.audio_dump:
    pcm = args.out / 'audio.pcm'
    result['pcm'] = dict(bytes=pcm.stat().st_size, sha256=fingerprint(pcm)) if pcm.exists() else None
    result['pcm_complete'] = bool(result['pcm'] and result['pcm']['bytes'] == args.frames * 532)
(args.out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result))
raise SystemExit(0 if result['completed'] and result['input_unchanged'] and result['binary_unchanged'] and result['case_verified'] and result.get('pcm_complete', True) else 1)
