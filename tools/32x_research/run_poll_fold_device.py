#!/usr/bin/env python3
"""Run one authorized S01 A/B/A; restore normal firmware and keep it awake."""
import argparse
import json
from pathlib import Path

from run_fixed_thumb_device import execute

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
a = parser.parse_args()
out = a.output.resolve()
result = json.loads((out / 'device-build-result.json').read_text())
assert result['status'] == 'built'
candidate = json.loads((out / 'device-arms/candidate/manifest.json').read_text())
assert candidate['variant'] == 'S01-compact'
assert candidate['compiler'] == result['compiler']
assert candidate['warmup'] == 900 and candidate['frames'] == 600
execute(out, attempt=1, rounds=1, keep_awake=True)
