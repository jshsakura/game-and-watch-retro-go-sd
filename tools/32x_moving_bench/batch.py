#!/usr/bin/env python3
"""Hold the device for a finite evidence-addressed batch. No polling/retries."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
sys.dont_write_bytecode=True
sys.path.insert(0,str(ROOT/'.agents/skills/gnw-ab-bench/scripts'))
from event_run import evidenced_batch, save


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--plan',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--timeout',type=int,required=True)
    p.add_argument('--lock',type=Path,default=Path('/tmp/gnw-swd.lock'))
    a=p.parse_args()
    if a.timeout<=0:p.error('finite positive budget required')
    plan=json.loads(a.plan.read_text())
    conditions=plan['evidence']['conditions']
    if conditions['environment'].get('device_preflight')!='verified':
        p.error('preflight incomplete: do not flash with unknown ROM/selector/save/ownership')
    if conditions['environment'].get('live_collector_validated') is not True:
        p.error('collector requires a separately authorized live smoke test first')
    a.output.mkdir(parents=True,exist_ok=True)
    job=Path(tempfile.mkdtemp(prefix='job-',dir=a.output))
    spec={'plan':plan,'cwd':str(ROOT),'timeout_seconds':a.timeout,
          'timeout_bin':shutil.which('timeout')}
    if not spec['timeout_bin']:p.error('GNU timeout required')
    save(job/'request.json',spec)
    with a.lock.open('a+b') as lock:
        try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError:
            save(job/'result.json',{'status':'device_busy','automatic_retry':False})
            raise SystemExit('device already owned; no wait/retry performed')
        os.environ['GNW_DEVICE_LOCK_HELD']='1'
        try:result=evidenced_batch(spec,job)
        except Exception as e:
            result={'status':'failed','error':str(e),'automatic_reset_or_retry':False}
        finally:os.environ.pop('GNW_DEVICE_LOCK_HELD',None)
        save(job/'result.json',result)
    print(json.dumps({'result':str(job/'result.json'),**result}))
    if result.get('exit_code',0) or result.get('status')=='failed':raise SystemExit(1)

if __name__=='__main__':main()
