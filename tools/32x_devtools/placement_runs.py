"""Backup -> run arms in SEQ, recording where the XIP payload and ROM landed in ext flash -> restore and start."""
from pathlib import Path
import subprocess,sys,json,hashlib,fcntl
r=Path('/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd');H=r/'build/32x-mixer-hle-20261001'
D=H/sys.argv[1];D.mkdir(exist_ok=False)
SEQ=sys.argv[2].split(',')
T=r/'tools/32x_device_bench';sys.path[:0]=[str(T),str(Path(__file__).resolve().parent)]
import run_arm_pc as run_arm,collect_place
run_arm.collect=collect_place.collect
ARMS=dict(kv.split('=') for kv in sys.argv[3].split(',')) if len(sys.argv)>3 else {'A':'hle2','B':'hle3'}
ARMS={k:H/'arms-bench'/v for k,v in ARMS.items()}
ALIGN=int(sys.argv[4]) if len(sys.argv)>4 else 0
WARM=int(sys.argv[5]) if len(sys.argv)>5 else 540
FRAMES=int(sys.argv[6]) if len(sys.argv)>6 else 180
SEL=Path(sys.argv[7]) if len(sys.argv)>7 else None   # selector file to push for the run
def place(elf):
    nm=dict((l.split()[2],int(l.split()[0],16)) for l in subprocess.run(['arm-none-eabi-nm',str(elf)],capture_output=True,text=True).stdout.splitlines() if len(l.split())==3)
    return dict(xip=nm['g_xip_addr'],rom=nm['md32x__Pico']+0x594,wptr=816,heap=nm.get('heap_end',0))
cmd=[sys.executable,str(T/'gnw_command.py')]
h=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
N=['intflash-bank2.bin','32x.bin','32x.xip','CONFIG','off.sav']
with open('/tmp/gnw-swd.lock','a') as lock:
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    B=D/'backup';B.mkdir()
    with (D/'backup.log').open('w') as log:
        subprocess.run(cmd+['dump','bank2','--dst',str(B/N[0]),'--size','262144',
            '--','sdpull','/cores/32x.bin',str(B/N[1]),'--','sdpull','/cores/32x.xip',str(B/N[2]),
            '--','sdpull','/CONFIG',str(B/N[3]),'--','sdpull','/data/off.sav',str(B/N[4]),'--','sdpull','/snes_bench_index.txt',str(B/'selector.txt')],stdout=log,stderr=subprocess.STDOUT,timeout=240,check=True)
    for n in N:
        if not (B/n).is_file() or not (B/n).stat().st_size: raise SystemExit('backup incomplete: '+n)
    assert h(B/'CONFIG')=='f918314a0f340f3a216e8032e54914b9a99777ea2f77675722c03def7f8e4883','CONFIG changed since preflight'
    print('BACKUP_OK',flush=True)
    if SEL:
        with (D/'selector-push.log').open('w') as log:
            subprocess.run(cmd+['sdpush','--file',str(SEL),'--dest-path','/snes_bench_index.txt','--','sdpull','/snes_bench_index.txt',str(D/'selector-readback.txt')],stdout=log,stderr=subprocess.STDOUT,timeout=120,check=True)
        assert (D/'selector-readback.txt').read_bytes()==SEL.read_bytes(),'selector readback differs'
        print('SELECTOR',SEL.read_bytes().decode(),flush=True)
    rows=[]
    try:
        for i,a in enumerate(SEQ):
            out=D/f'{i:02d}-{a}'
            collect_place.PLACE.clear();collect_place.PLACE.update(place(ARMS[a]/'gw_retro_go.elf'))
            run_arm.run(ARMS[a],out,240,WARM,FRAMES)
            rep=json.loads((out/'capture/report.json').read_text());pl=json.loads((out/'capture/placement.json').read_text())
            row=dict(i=i,arm=a,fps=round(rep['emu_fps'],3),elapsed_ms=rep['elapsed_ms'],start_ms=rep['start_ms'],
                     audio=rep['audio_sha256'][:8],screen=rep['screen_sha256'][:8],**{k:(hex(v) if isinstance(v,int) else v) for k,v in pl.items()})
            rows.append(row);print(json.dumps(row),flush=True)
            if ALIGN and pl['xip_addr'] % ALIGN: raise SystemExit(f'ARM {i}-{a} NOT ALIGNED: xip at {pl["xip_addr"]:#x}')
        import compare
        ld=lambda i,a:compare.load(D/f'{i:02d}-{a}'/'capture/report.json')
        for t in range(0,len(SEQ)-2,2):
            if SEQ[t:t+3]!=['A','B','A']: continue
            try: res=compare.compare(ld(t,'A'),ld(t+1,'B'),ld(t+2,'A'),0.5)
            except ValueError as e: res={'status':'invalid','reason':str(e)}
            (D/f'comparison-{t:02d}.json').write_text(json.dumps(res,indent=2));print('COMPARE',t,json.dumps(res),flush=True)
    finally:
        (D/'rows.json').write_text(json.dumps(rows,indent=1))
        R=D/'restore';R.mkdir()
        with (R/'restore.log').open('w') as log:
            subprocess.run(cmd+['flash','bank2',str(B/N[0]),'--','sdpush','--file',str(B/N[1]),'--dest-path','/cores/',
                '--','sdpush','--file',str(B/N[2]),'--dest-path','/cores/','--','sdpush','--file',str(B/N[3]),'--dest-path','/CONFIG',
                '--','sdpull','/cores/32x.bin',str(R/N[1]),'--','sdpull','/cores/32x.xip',str(R/N[2]),'--','sdpull','/CONFIG',str(R/N[3]),
                '--','sdpush','--file',str(B/'selector.txt'),'--dest-path','/snes_bench_index.txt','--','sdpull','/snes_bench_index.txt',str(R/'selector.txt'),'--','sdpull','/data/off.sav',str(R/N[4]),'--','dump','bank2','--dst',str(R/N[0]),'--size','262144'],stdout=log,stderr=subprocess.STDOUT,timeout=300,check=True)
        checks={n:h(R/n)==h(B/n) for n in N+['selector.txt']}
        if not all(checks.values()): raise SystemExit('RESTORE_MISMATCH '+json.dumps(checks))
        with (R/'start.log').open('w') as log: subprocess.run(cmd+['start','bank2'],stdout=log,stderr=subprocess.STDOUT,timeout=30,check=True)
        print('RESTORE_OK',flush=True)
