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
args = ap.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
elf, rom = args.elf.resolve(), args.rom.resolve()
fingerprint = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
request = dict(elf=str(elf), elf_sha256=fingerprint(elf), rom=str(rom),
               rom_sha256=fingerprint(rom), save=None, input='none',
               workload='1200 frames from cold boot; gate forced armed',
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
started = time.monotonic()
with (args.out / 'output.log').open('w') as output:
    try:
        result = subprocess.run(argv, stdout=output, stderr=subprocess.STDOUT,
                                timeout=args.timeout, check=False)
        code = result.returncode
    except subprocess.TimeoutExpired:
        code = 124
elapsed = time.monotonic() - started
log = (args.out / 'output.log').read_text()
done = re.search(r'done (\d+) frames STATEHASH=(\w+) AUDIOHASH=(\w+) '
                 r'avg emu=(\d+) apu=(\d+) insn/frame', log)
result = dict(exit_code=code, elapsed_seconds=round(elapsed, 3),
              completed=bool(code == 0 and done), input_unchanged=fingerprint(rom) == request['rom_sha256'],
              binary_unchanged=fingerprint(elf) == request['elf_sha256'])
if done:
    result.update(frames=int(done[1]), statehash=done[2], audiohash=done[3],
                  emu_insn_per_frame=int(done[4]), apu_insn_per_frame=int(done[5]))
else:
    result['failure_tail'] = log.splitlines()[-6:]
(args.out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result))
raise SystemExit(0 if result['completed'] and result['input_unchanged'] and result['binary_unchanged'] else 1)
