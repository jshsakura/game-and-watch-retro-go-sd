#!/usr/bin/env python3
"""Flash one authorized immutable arm, verify all parts, start once and collect."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from collect import collect

NAMES = ['gw_retro_go.elf','gw_retro_go_intflash.bin','32x.bin','32x.xip']


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(arm, output, timeout, warmup, frames, smoke=False):
    manifest=json.loads((arm/'manifest.json').read_text())
    for name in NAMES:
        if not (arm/name).is_file() or not (arm/name).stat().st_size:
            raise ValueError('missing artifact: '+name)
        if digest(arm/name) != manifest['sha256'][name]:
            raise ValueError('artifact identity changed: '+name)
    if manifest['warmup'] != warmup or manifest['frames'] != frames:
        raise ValueError('binary window differs from requested window')
    if manifest.get('profiler') not in (False, 'pc-raw') or manifest.get('device_hook') != 1:
        raise ValueError('unexpected measurement instrumentation')
    output.mkdir(parents=True,exist_ok=False)
    verify=output/'verification';verify.mkdir()
    driver=Path('/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/tools/32x_device_bench/gnw_command.py')
    args=[sys.executable,str(driver),'flash','bank2',str(arm/NAMES[1])]
    for name in ['32x.bin','32x.xip']:
        args+=['--','sdpush','--file',str(arm/name),'--dest-path','/cores/']
        args+=['--','sdpull','/cores/'+name,str(verify/name)]
    args+=['--','dump','bank2','--dst',str(verify/NAMES[1]),
           '--size',str((arm/NAMES[1]).stat().st_size)]
    with (output/'flash.log').open('w') as log:
        subprocess.run(args,stdout=log,stderr=subprocess.STDOUT,timeout=180,check=True)
    for name in NAMES[1:]:
        if digest(verify/name) != manifest['sha256'][name]:
            raise ValueError('device readback mismatch: '+name)
    # The boot occurs only after matching readbacks. No retries on boot failure.
    with (output/'start.log').open('w') as log:
        subprocess.run([sys.executable,str(driver),'start','bank2'],
                       stdout=log,stderr=subprocess.STDOUT,timeout=30,check=True)
    r=collect(arm/NAMES[0],output/'capture',timeout,3333,warmup,smoke)
    if manifest.get('silent_completion_required') and not r.get('audio_dma_stopped'):
        raise ValueError('audio transmitter or DMA still enabled at completed breakpoint')
    if r['warmup']!=warmup or r['window']!=frames:
        raise ValueError('device used a different frame window')
    for name in NAMES:
        if digest(arm/name)!=manifest['sha256'][name]:
            raise ValueError('input changed during run: '+name)
    (output/'result.json').write_text(json.dumps({'status':'completed',
        'manifest':manifest,'capture':str(output/'capture/report.json')},indent=2)+'\n')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--arm',type=Path,required=True)
    p.add_argument('--name',choices=['base-before','candidate','base-after'],required=True)
    p.add_argument('--warmup',type=int,required=True)
    p.add_argument('--frames',type=int,required=True)
    p.add_argument('--timeout',type=int,default=240)
    a=p.parse_args()
    # The outer batch must hold the global device lock and fix the workload.
    if os.environ.get('GNW_DEVICE_LOCK_HELD') != '1':
        p.error('use batch.py; global device ownership must cover the entire A/B/A')
    output=Path(os.environ['GNW_AB_ROUND_DIR'])/a.name
    if output.exists():
        p.error('output already exists; preserve prior evidence')
    try:
        run(a.arm.resolve(strict=True),output,a.timeout,a.warmup,a.frames)
    except Exception as e:
        output.mkdir(parents=True,exist_ok=True)
        (output/'failure.json').write_text(json.dumps({'status':'failed','error':str(e),
            'automatic_reset_or_retry':False},indent=2)+'\n')
        raise SystemExit(str(e))

if __name__=='__main__':main()
