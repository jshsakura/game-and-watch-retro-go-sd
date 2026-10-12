#!/usr/bin/env python3
import fcntl,json,subprocess,sys,hashlib
from pathlib import Path
from run_arm import run
r=Path(__file__).resolve().parents[2];e=r/'build/32x-device-20260930';o=e/'pc-diagnostic3';o.mkdir(exist_ok=False)
h=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
with open('/tmp/gnw-swd.lock','a') as lock:
 fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
 with (o/'preflight.log').open('w') as log:
  subprocess.run([sys.executable,str(r/'tools/32x_device_bench/gnw_command.py'),'sdpull','/CONFIG',str(o/'CONFIG'),'--','sdpull','/data/off.sav',str(o/'off.sav'),'--','sdpush','--file',str(e/'selector.doom.txt'),'--dest-path','/snes_bench_index.txt','--','sdpull','/snes_bench_index.txt',str(o/'selector.txt')],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
 checks={'config':h(o/'CONFIG')==h(e/'backup/CONFIG'),'save':h(o/'off.sav')==h(e/'backup/off.sav'),'selector':(o/'selector.txt').read_bytes()==(e/'selector.doom.txt').read_bytes()}
 (o/'preflight.json').write_text(json.dumps(checks,indent=2)+'\n')
 if not all(checks.values()):raise ValueError('diagnostic conditions changed')
 run(e/'arms-pc-sample/diagnostic3',o/'run',240,900,600)
 report=json.loads((o/'run/capture/report.json').read_text());prior=json.loads((e/'gameplay-summary.json').read_text())
 expected={'screen_sha256':'dacd911185b129c66d5307a515a683abe96079c29d27b88a53edd1800354dac6','audio_sha256':'13087d2b2afe5b8ca9d213d2f85399109efba4b3545150bea253479fe699bed6','last_drawn_frame':1500,'delta_drawn':600,'delta_emu':600,'delta_audio_samples':441000,'clock_hz':340000000,'fullscreen':0,'tear_guard':0}
 checks={k:report[k]==v for k,v in expected.items()}
 result={'status':'completed' if all(checks.values()) else 'endpoint_mismatch','checks':checks,'diagnostic':report['pc_diagnostic'],'candidate_fps':False}
 (o/'result.json').write_text(json.dumps(result,indent=2)+'\n')
 if not all(checks.values()):raise ValueError('diagnostic workload differs')
print(json.dumps({'status':'pc_diagnostic_completed','result':str(o/'result.json')}))
