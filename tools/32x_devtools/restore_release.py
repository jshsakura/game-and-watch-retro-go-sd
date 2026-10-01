"""Put the device back to what install_release.py backed up (bank2 firmware + every file it overwrote).
usage: restore_release.py <install dir>   (the one with plan.json and backup/)"""
from pathlib import Path
import subprocess,sys,json,hashlib,fcntl
H=Path('/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/build/32x-mixer-hle-20261001');R=H.parent.parent
D=H/sys.argv[1];B=D/'backup';plan=json.loads((D/'plan.json').read_text());man=json.loads((D/'backup-manifest.json').read_text())
cmd=[sys.executable,str(R/'tools/32x_device_bench/gnw_command.py')]
h=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
for f,v in man.items():
    if h(B/f)!=v: raise SystemExit('backup changed on disk: '+f)
with open('/tmp/gnw-swd.lock','a') as lock:
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    a=cmd+['flash','bank2',str(B/'intflash-bank2.bin')]
    for f in plan['overwrites']: a+=['--','sdpush','--file',str(B/f),'--dest-path','/'+f]
    for f in plan['new']: a+=['--','sdrm','/'+f]
    V=D/'restore-readback';V.mkdir(exist_ok=True)
    a+=['--','dump','bank2','--dst',str(V/'intflash-bank2.bin'),'--size','262144']
    for f in plan['overwrites']:
        (V/Path(f).parent).mkdir(parents=True,exist_ok=True); a+=['--','sdpull','/'+f,str(V/f)]
    with (D/'restore.log').open('w') as log: subprocess.run(a,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=2400)
    bad=[f for f in plan['overwrites']+['intflash-bank2.bin'] if h(V/f)!=man[f]]
    if bad: raise SystemExit('RESTORE_MISMATCH '+str(bad))
    subprocess.run(cmd+['start','bank2'],check=True,timeout=60)
    print('RESTORED and started:',len(plan['overwrites']),'files + bank2 verified')
