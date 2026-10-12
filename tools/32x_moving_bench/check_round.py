#!/usr/bin/env python3
"""Check one completed triplet from the existing batch executor."""
import argparse
import json
import os
from pathlib import Path
from compare import load, compare

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--max-drift-percent',type=float,required=True)
a=p.parse_args()
if not 0<=a.max_drift_percent<100:p.error('invalid drift bound')
root=Path(os.environ['GNW_AB_ROUND_DIR'])
try:
    reports=[load(root/n/'capture/report.json') for n in ['base-before','candidate','base-after']]
    result=compare(*reports,a.max_drift_percent);code=0
except (ValueError,KeyError,OSError) as e:
    result={'status':'invalid','reason':str(e)};code=1
(root/'comparison.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result));raise SystemExit(code)
