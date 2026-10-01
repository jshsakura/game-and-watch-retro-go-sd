#!/usr/bin/env python3
"""Compare completed SNES rig logs; instruction savings are not device FPS."""
import argparse
import json
from pathlib import Path
import re

ap = argparse.ArgumentParser()
ap.add_argument('base', type=Path)
ap.add_argument('candidate', type=Path)
args = ap.parse_args()
pattern = re.compile(r'w(\d+) emu=(\d+) apu=(\d+) insn/frame fb=(\w+) audio=(\w+) lit=(\d+)')
final = re.compile(r'done (\d+) frames STATEHASH=(\w+) AUDIOHASH=(\w+) avg emu=(\d+) apu=(\d+) insn/frame')
logs = [p.read_text() for p in (args.base, args.candidate)]
ends = [final.search(log) for log in logs]
windows = [[m.groups() for m in pattern.finditer(log)] for log in logs]
if not all(ends) or not windows[0] or len(windows[0]) != len(windows[1]):
    raise SystemExit('incomplete or incompatible logs')
passed = ends[0].groups()[:3] == ends[1].groups()[:3]
rows = []
for a, b in zip(*windows):
    identical = a[0] == b[0] and a[3:] == b[3:]
    passed &= identical
    total_a, total_b = int(a[1]) + int(a[2]), int(b[1]) + int(b[2])
    rows.append(dict(end_frame=int(a[0]), hashes_equal=identical,
                     base_insn=total_a, candidate_insn=total_b,
                     reduction_percent=round(100 * (1 - total_b / total_a), 4)))
a, b = ends
total_a, total_b = int(a[4]) + int(a[5]), int(b[4]) + int(b[5])
result = dict(correctness='PASS' if passed else 'FAIL', frames=int(a[1]),
              statehash=a[2], audiohash=a[3], base_insn=total_a,
              candidate_insn=total_b,
              reduction_percent=round(100 * (1 - total_b / total_a), 4),
              windows=rows, device_fps='unmeasured')
print(json.dumps(result, indent=2))
raise SystemExit(0 if passed else 1)
