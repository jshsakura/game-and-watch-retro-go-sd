#!/usr/bin/env python3
"""Compare completed SNES rig logs; instruction savings are not device FPS."""
import argparse
import json
from pathlib import Path
import re
import statistics

ap = argparse.ArgumentParser()
ap.add_argument('base', type=Path)
ap.add_argument('candidate', type=Path)
args = ap.parse_args()
if args.base.resolve() == args.candidate.resolve():
    raise SystemExit('base and candidate must be distinct executions')
pattern = re.compile(r'w(\d+) emu=(\d+) apu=(\d+) insn/frame fb=(\w+) audio=(\w+) lit=(\d+)')
final = re.compile(r'done (\d+) frames STATEHASH=(\w+) AUDIOHASH=(\w+) avg emu=(\d+) apu=(\d+) insn/frame')
logs = [p.read_text() for p in (args.base, args.candidate)]
ends = [final.search(log) for log in logs]
windows = [[m.groups() for m in pattern.finditer(log)] for log in logs]
if not all(ends) or any(len(final.findall(log)) != 1 for log in logs) or not windows[0] or len(windows[0]) != len(windows[1]):
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
trace_pattern = re.compile(r'\[frame\] (\d+) emu=(\d+) apu=(\d+) total=(\d+) fb=(\w+) audio=(\w+)')
traces = [[m.groups() for m in trace_pattern.finditer(log)] for log in logs]
if any(traces):
    expected = int(a[1])
    trace_ok = all(len(t) == expected and [int(r[0]) for r in t] == list(range(1, expected + 1)) for t in traces)
    trace_ok &= all(x[0] == y[0] and x[4:] == y[4:] for x, y in zip(*traces))
    passed &= trace_ok
    result['per_frame_output_equal'] = trace_ok
    if trace_ok:
        def distribution(values):
            ordered = sorted(values)
            return dict(n=len(values), mean=round(statistics.mean(values), 2),
                        **{f'p{p}': ordered[min(len(ordered) - 1, p * len(ordered) // 100)] for p in (50, 90, 95, 99)},
                        min=ordered[0], max=ordered[-1])
        costs = [[int(row[3]) for row in trace] for trace in traces]
        result['total_distribution'] = dict(base=distribution(costs[0]), candidate=distribution(costs[1]))
        result['total_reduction_percent'] = round(100 * (1 - sum(costs[1]) / sum(costs[0])), 4)
        result['last_300_distribution'] = dict(base=distribution(costs[0][-300:]), candidate=distribution(costs[1][-300:]))
        worst = sorted(range(expected), key=lambda i: costs[0][i], reverse=True)[:10]
        result['baseline_heaviest_10'] = [dict(frame=i + 1, base=costs[0][i], candidate=costs[1][i],
                                             reduction_percent=round(100 * (1 - costs[1][i] / costs[0][i]), 4)) for i in worst]
        result['regressed_frames_over_1pct'] = sum(y > x * 1.01 for x, y in zip(*costs))
state_pattern = re.compile(r'\[state\] bytes=(\d+) hash=([0-9a-f]{16})')
states = [state_pattern.search(log) for log in logs]
if any(states):
    same = bool(all(states) and states[0].groups() == states[1].groups())
    passed &= same
    result['serialized_state_equal'] = same
    result['serialized_state'] = [dict(bytes=int(s[1]), hash=s[2]) if s else None for s in states]
run_paths = [p.parent / 'result.json' for p in (args.base, args.candidate)]
if all(p.exists() for p in run_paths):
    runs = [json.loads(p.read_text()) for p in run_paths]
    valid = all(r.get('completed') and r.get('binary_unchanged') and r.get('input_unchanged') and r.get('case_verified', True) for r in runs)
    valid &= all(r.get('exit_code', 0) == 0 and r.get('frames', int(a[1])) == int(a[1]) for r in runs)
    passed &= valid
    result['execution_records_valid'] = valid
    requests = [json.loads((p.parent / 'request.json').read_text()) for p in run_paths]
    matched = all(requests[0].get(k) == requests[1].get(k) for k in ('rom_sha256', 'save', 'input', 'frames', 'gate', 'case_config', 'audio_dump'))
    passed &= matched
    result['workload_identity_equal'] = matched
    if any('pcm' in r for r in runs):
        same = all(r.get('pcm_complete') for r in runs) and runs[0].get('pcm') == runs[1].get('pcm')
        passed &= same
        result['pcm_equal'] = same
        result['pcm'] = [r.get('pcm') for r in runs]
result['correctness'] = 'PASS' if passed else 'FAIL'
print(json.dumps(result, indent=2))
raise SystemExit(0 if passed else 1)
