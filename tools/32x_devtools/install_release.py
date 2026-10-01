"""Install a release build on the device: bank2 firmware + the cores / lang / homebrew files that carry its tag.
Backs up everything it will overwrite first, reads everything back after, and restores the backup if any step fails.
usage: install_release.py <outdir> <release worktree> [--dry-run]"""
from pathlib import Path
import subprocess,sys,json,hashlib,fcntl,struct,re,os,time
H=Path('/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/build/32x-mixer-hle-20261001');R=H.parent.parent
D=H/sys.argv[1];W=Path(sys.argv[2]).resolve();DRY='--dry-run' in sys.argv
cmd=[sys.executable,str(R/'tools/32x_device_bench/gnw_command.py')]
h=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
SC=W/'sd_content';IMG=W/'build/gw_retro_go_intflash.bin'
tag=re.search(r'#define GIT_TAG "(.*)"',(W/'Core/Inc/gittag.h').read_text()).group(1)
assert tag.encode() in IMG.read_bytes(),'firmware image does not carry the tag'
assert not tag.endswith('+'),'dirty tree stamp'
def files(sub):
    return sorted(str(p.relative_to(SC)) for p in (SC/sub).rglob('*') if p.is_file())
INSTALL=files('cores')+files('lang')+files('roms/homebrew')
# every internal core must carry exactly this firmware's tag, or the launcher shows "corrupted installation"
bad=[]
for f in INSTALL:
    if not (f.startswith('cores/') and f.endswith('.bin')): continue
    d=(SC/f).read_bytes()[:64]
    if d[:4]==b'CORI':
        n=d[8];t=d[9:9+n].decode()
        if t!=tag: bad.append((f,t))
print('tag:',tag);print('files to install:',len(INSTALL),'(cores %d, lang %d, homebrew %d)'%(len(files('cores')),len(files('lang')),len(files('roms/homebrew'))))
print('internal cores with a different tag:',bad or 'none')
if bad: raise SystemExit('tag mismatch')
if DRY: raise SystemExit('dry run: nothing touched')
def run(a,log,t): subprocess.run(a,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=t)
def sdls(p):
    r=subprocess.run(cmd+['sdls',p],capture_output=True,text=True,timeout=120,check=True)
    return {l.strip() for l in re.sub(r'\x1b\[[0-9;]*m','',r.stdout).splitlines() if l.strip()}
D.mkdir(exist_ok=False);B=D/'backup';B.mkdir()
with open('/tmp/gnw-swd.lock','a') as lock:
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    # what exists on the card now (only those can be backed up / restored; new ones are removed again on restore)
    exist={};
    for sub in ['/cores','/cores/mappers','/lang','/roms/homebrew']:
        exist[sub]=sdls(sub)
    on_card=[f for f in INSTALL if '/'+os.path.basename(f) in {'/'+n for n in exist['/'+os.path.dirname(f)]}]
    new_files=[f for f in INSTALL if f not in on_card]
    json.dump({'tag':tag,'install':INSTALL,'overwrites':on_card,'new':new_files},open(D/'plan.json','w'),indent=1)
    print('overwrites',len(on_card),'new',len(new_files),flush=True)
    # 1. backup: bank2 + every file we will overwrite
    args=cmd+['dump','bank2','--dst',str(B/'intflash-bank2.bin'),'--size','262144']
    for f in on_card:
        (B/Path(f).parent).mkdir(parents=True,exist_ok=True)
        args+=['--','sdpull','/'+f,str(B/f)]
    args+=['--','sdpull','/CONFIG',str(B/'CONFIG')]
    with (D/'backup.log').open('w') as log: run(args,log,1500)
    man={f:h(B/f) for f in on_card+['intflash-bank2.bin','CONFIG']}
    for f,v in man.items():
        if not (B/f).is_file() or not (B/f).stat().st_size: raise SystemExit('backup incomplete: '+f)
    json.dump(man,open(D/'backup-manifest.json','w'),indent=1);print('BACKUP_OK',len(man),'files',flush=True)
    def restore():
        a=cmd+['flash','bank2',str(B/'intflash-bank2.bin')]
        for f in on_card: a+=['--','sdpush','--file',str(B/f),'--dest-path','/'+f]
        for f in new_files: a+=['--','sdrm','/'+f]
        with (D/'restore.log').open('w') as log: run(a,log,1500)
        subprocess.run(cmd+['start','bank2'],timeout=60)
    try:
        # 2. install: firmware first, then the files that carry its tag
        a=cmd+['flash','bank2',str(IMG)]
        for f in INSTALL: a+=['--','sdpush','--file',str(SC/f),'--dest-path','/'+f]
        with (D/'install.log').open('w') as log: run(a,log,2400)
        print('INSTALL_DONE',flush=True)
        # 3. read everything back and compare
        V=D/'readback';V.mkdir()
        a=cmd+['dump','bank2','--dst',str(V/'intflash-bank2.bin'),'--size',str(IMG.stat().st_size)]
        for f in INSTALL:
            (V/Path(f).parent).mkdir(parents=True,exist_ok=True); a+=['--','sdpull','/'+f,str(V/f)]
        with (D/'readback.log').open('w') as log: run(a,log,2400)
        checks={f:h(V/f)==h(SC/f) for f in INSTALL}; checks['bank2']=h(V/'intflash-bank2.bin')==h(IMG)
        json.dump(checks,open(D/'readback.json','w'),indent=1)
        if not all(checks.values()): raise RuntimeError('readback mismatch: '+str([k for k,v in checks.items() if not v]))
        print('READBACK_OK',len(checks),'files identical',flush=True)
    except Exception as e:
        print('FAILED, restoring backup:',e,flush=True); restore(); print('RESTORED',flush=True); raise
    with (D/'start.log').open('w') as log: run(cmd+['start','bank2'],log,60)
    print('STARTED',flush=True)
