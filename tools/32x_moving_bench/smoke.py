#!/usr/bin/env python3
"""One authorized live meter validation, held under the global device lock."""
import argparse
import fcntl
import json
from pathlib import Path
from run_arm import run

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--arm',type=Path,required=True)
p.add_argument('--conditions',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--timeout',type=int,default=240)
p.add_argument('--lock',type=Path,default=Path('/tmp/gnw-swd.lock'))
a=p.parse_args()
c=json.loads(a.conditions.read_text())
if c['environment'].get('device_preflight')!='verified':p.error('preflight not verified')
if a.timeout<=0 or c['workload']['warmup']<=0 or c['workload']['frames']<=0:p.error('invalid window/deadline')
if a.output.exists():p.error('output exists; preserve evidence')
with a.lock.open('a+b') as lock:
    try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    except BlockingIOError:raise SystemExit('device busy; no wait/retry')
    try:
        run(a.arm.resolve(strict=True),a.output.resolve(),a.timeout,
            c['workload']['warmup'],c['workload']['frames'],smoke=True)
    except Exception as e:
        a.output.mkdir(parents=True,exist_ok=True)
        (a.output/'failure.json').write_text(json.dumps({'status':'failed','error':str(e),
            'automatic_reset_or_retry':False},indent=2)+'\n')
        raise SystemExit(str(e))
print(json.dumps({'status':'live_smoke_completed','result':str(a.output/'result.json')}))
