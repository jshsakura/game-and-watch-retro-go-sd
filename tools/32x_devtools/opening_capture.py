"""Flash arm(s), boot Doom, and photograph the panel continuously from boot (LTDC scanout, CPU never halted).
usage: opening_capture.py <outdir> <arm,arm,..> <name=armdir,...> <selector> <frames> <gap_ms>
Backs up bank2/cores/CONFIG/off.sav/selector first and restores + reads them back at the end."""
from pathlib import Path
import subprocess,sys,json,hashlib,fcntl,time,os
r=Path('/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd');H=r/'build/32x-mixer-hle-20261001'
D=H/sys.argv[1];D.mkdir(exist_ok=False)
SEQ=sys.argv[2].split(',');ARMS={k:H/v for k,v in (kv.split('=') for kv in sys.argv[3].split(','))}
SEL=Path(sys.argv[4]);NF=int(sys.argv[5]);GAP=int(sys.argv[6])
cmd=[sys.executable,str(r/'tools/32x_device_bench/gnw_command.py')]
h=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
N=['intflash-bank2.bin','32x.bin','32x.xip','CONFIG','off.sav','selector.txt']
def run(a,**k): return subprocess.run(a,stdout=k.pop('log'),stderr=subprocess.STDOUT,check=True,**k)
with open('/tmp/gnw-swd.lock','a') as lock:
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    B=D/'backup';B.mkdir()
    with (D/'backup.log').open('w') as log:
        run(cmd+['dump','bank2','--dst',str(B/N[0]),'--size','262144','--','sdpull','/cores/32x.bin',str(B/N[1]),'--','sdpull','/cores/32x.xip',str(B/N[2]),
            '--','sdpull','/CONFIG',str(B/N[3]),'--','sdpull','/data/off.sav',str(B/N[4]),'--','sdpull','/snes_bench_index.txt',str(B/N[5])],log=log,timeout=240)
    for n in N:
        if not (B/n).is_file() or not (B/n).stat().st_size: raise SystemExit('backup incomplete: '+n)
    print('BACKUP_OK',flush=True)
    try:
        with (D/'selector.log').open('w') as log:
            run(cmd+['sdpush','--file',str(SEL),'--dest-path','/snes_bench_index.txt','--','sdpull','/snes_bench_index.txt',str(D/'selector-rb.txt')],log=log,timeout=120)
        assert (D/'selector-rb.txt').read_bytes()==SEL.read_bytes()
        for i,a in enumerate(SEQ):
            arm=ARMS[a];m=json.loads((arm/'manifest.json').read_text());o=D/f'{i:02d}-{a}';o.mkdir()
            for n in ['gw_retro_go_intflash.bin','32x.bin','32x.xip']:
                if h(arm/n)!=m['sha256'][n]: raise SystemExit('artifact identity changed '+n)
            args=cmd+['flash','bank2',str(arm/'gw_retro_go_intflash.bin')]
            for n in ['32x.bin','32x.xip']: args+=['--','sdpush','--file',str(arm/n),'--dest-path','/cores/','--','sdpull','/cores/'+n,str(o/('rb-'+n))]
            args+=['--','dump','bank2','--dst',str(o/'rb-intflash.bin'),'--size',str((arm/'gw_retro_go_intflash.bin').stat().st_size)]
            with (o/'flash.log').open('w') as log: run(args,log=log,timeout=240)
            for n,rb in [('gw_retro_go_intflash.bin','rb-intflash.bin'),('32x.bin','rb-32x.bin'),('32x.xip','rb-32x.xip')]:
                if h(o/rb)!=m['sha256'][n]: raise SystemExit('device readback mismatch '+n)
            tcl=o/'burst.tcl'
            tcl.write_text(f'''for {{set i 0}} {{$i < {NF}}} {{incr i}} {{
  set t [clock milliseconds]
  set fb [lindex [read_memory 0x500010AC 32 1] 0]
  dump_image {o}/f[format %03d $i].bin $fb 153600
  puts "FRAME $i fb [format 0x%08x $fb] t $t"
  after {GAP}
}}
''')
            with (o/'start.log').open('w') as log: run(cmd+['start','bank2'],log=log,timeout=30)
            t0=time.time()
            with (o/'burst.log').open('w') as log:
                subprocess.run(['openocd','-f','interface/stlink-dap.cfg','-f','target/stm32h7x.cfg','-c','adapter speed 4000',
                    '-c','stm32h7x.cpu0 configure -work-area-size 0','-c','tcl_port disabled','-c','telnet_port disabled','-c','gdb_port disabled',
                    '-c','init','-c',f'source {tcl}','-c','shutdown'],stdout=log,stderr=subprocess.STDOUT,timeout=NF*8+60,check=True)
            print(f'ARM {a} captured {len(list(o.glob("f*.bin")))} frames in {time.time()-t0:.0f}s',flush=True)
    finally:
        R=D/'restore';R.mkdir()
        with (R/'restore.log').open('w') as log:
            run(cmd+['flash','bank2',str(B/N[0]),'--','sdpush','--file',str(B/N[1]),'--dest-path','/cores/','--','sdpush','--file',str(B/N[2]),'--dest-path','/cores/',
                '--','sdpush','--file',str(B/N[3]),'--dest-path','/CONFIG','--','sdpush','--file',str(B/N[5]),'--dest-path','/snes_bench_index.txt',
                '--','sdpull','/cores/32x.bin',str(R/N[1]),'--','sdpull','/cores/32x.xip',str(R/N[2]),'--','sdpull','/CONFIG',str(R/N[3]),'--','sdpull','/data/off.sav',str(R/N[4]),
                '--','sdpull','/snes_bench_index.txt',str(R/N[5]),'--','dump','bank2','--dst',str(R/N[0]),'--size','262144'],log=log,timeout=300)
        checks={n:h(R/n)==h(B/n) for n in N}
        (R/'readback.json').write_text(json.dumps(checks,indent=2))
        if not all(checks.values()): raise SystemExit('RESTORE_MISMATCH '+json.dumps(checks))
        with (R/'start.log').open('w') as log: run(cmd+['start','bank2'],log=log,timeout=30)
        print('RESTORE_OK',flush=True)
