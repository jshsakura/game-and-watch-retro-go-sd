#!/usr/bin/env python3
"""Validate completed A/B/A records, then report FPS and baseline drift."""
import argparse
import hashlib
import json
from pathlib import Path
from collect import decode


def load(path):
    r = json.loads(path.read_text())
    decoded = decode((path.parent / 'record.bin').read_bytes())
    for k, v in decoded.items():
        if r.get(k) != v:
            raise ValueError('record/report mismatch: ' + k)
    for name in ['screen', 'audio']:
        data=(path.parent/(name+'.bin')).read_bytes()
        if not data or (name=='screen' and len(data)!=153600):
            raise ValueError('incomplete '+name+' dump')
        if hashlib.sha256(data).hexdigest() != r[name+'_sha256']:
            raise ValueError('probe/report hash mismatch: ' + name)
    return r


def compare(a, b, c, max_drift):
    for k in ['warmup','window','clock_hz','fullscreen','tear_guard', 'input_or',
              'delta_audio_calls','delta_audio_samples','delta_muted_calls',
              'last_drawn_frame', 'audio_sha256','screen_sha256']:
        if not a[k] == b[k] == c[k]:
            raise ValueError('A/B/A mismatch: ' + k)
    if a['elf_sha256'] != c['elf_sha256'] or a['elf_sha256'] == b['elf_sha256']:
        raise ValueError('A/B/A binary identity invalid')
    afps, bfps, cfps = (r['drawn_fps'] for r in [a,b,c])
    base = (afps + cfps) / 2
    drift = (cfps / afps - 1) * 100
    if abs(drift) > max_drift:
        raise ValueError(f'baseline drift {drift:.5f}% exceeds {max_drift}%')
    gain = (bfps/base-1)*100
    return dict(status='valid_descriptive', baseline_before=afps,
        candidate=bfps, baseline_after=cfps, drift_percent=drift,
        gain_percent=gain, candidate_faster_than_both=bfps>max(afps,cfps),
        candidate_exceeds_drift=gain>abs(drift),
        limitation='one triplet: no statistical significance or full-stream correctness claim')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ['before','candidate','after','output']:
        p.add_argument('--'+n, type=Path, required=True)
    p.add_argument('--max-drift-percent', type=float, required=True)
    a=p.parse_args()
    if not 0 <= a.max_drift_percent < 100: p.error('invalid drift bound')
    try:
        r=compare(*[load(x) for x in [a.before,a.candidate,a.after]], a.max_drift_percent)
        code=0
    except (ValueError,KeyError,OSError) as e:
        r=dict(status='invalid',reason=str(e));code=1
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(r,indent=2)+'\n')
    print(json.dumps(r));raise SystemExit(code)

if __name__=='__main__':main()
