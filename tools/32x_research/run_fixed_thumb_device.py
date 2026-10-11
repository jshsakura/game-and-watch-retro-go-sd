#!/usr/bin/env python3
"""Finite, muted Doom A/B/A with backup/readback/normal restore; no polling."""
import argparse,fcntl,hashlib,json,re,struct,subprocess,sys,tempfile,traceback,zlib
from pathlib import Path
import run_aot_blocks as rig

ROOT=rig.ROOT
DRIVER=[sys.executable,str(ROOT/'tools/32x_device_bench/gnw_command.py')]

def execute(out,attempt=1,rounds=2,keep_awake=False):
    sys.path.insert(0,str(out/'device-tools'))
    import run_arm,compare
    evidence=out/f'device-{attempt:06d}';evidence.mkdir(exist_ok=False)
    r=dict(status='preparing',automatic_reset_or_retry=False,restored=False,comparisons=[],
           workload=dict(cold_boot=True,load_state=False,input=0,warmup=900,frames=600,volume=0,
                         startup_input='B f600..607,720..727,840..847',scene='Doom level entry; completion screen must be inspected'))
    def command(args,name,timeout):
        with (evidence/name).open('w') as log:
            subprocess.run(DRIVER+args,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=timeout,start_new_session=True)
    def listing(path,name):
        p=subprocess.run(DRIVER+['sdls',path],capture_output=True,text=True,timeout=120,start_new_session=True)
        (evidence/name).write_text(p.stdout+p.stderr);p.check_returncode()
        return {s.strip() for s in re.sub(r'\x1b\[[0-9;]*m','',p.stdout).splitlines() if s.strip()}
    def state(status,detail):
        s=json.loads((out/'state.json').read_text());s.update(status=status,last_result=str(evidence/'result.json'),device_state=detail)
        rig.save(out/'state.json',s)
    try:
        build=json.loads((out/'device-build-result.json').read_text());assert build['status']=='built'
        arms={n:out/'device-arms'/n for n in ('baseline','candidate')}
        manifests={n:json.loads((p/'manifest.json').read_text()) for n,p in arms.items()}
        assert all(m['volume']==0 and m['warmup']==900 and m['frames']==600 for m in manifests.values())
        with open('/tmp/gnw-swd.lock','a+') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            initial=listing('/','root-before.log');roms=listing('/roms/32x','roms.log')
            names=sorted(n for n in roms if n.lower().endswith('.32x') and re.search(r'doom|둠',n,re.I) and not re.search(r'd32xr',n,re.I))
            assert names,'No Doom ROM found'
            expected=json.loads((out/'doom-baseline/inputs.json').read_text())['rom_sha256']
            matches=[];identities={}
            with tempfile.TemporaryDirectory(prefix='gnw-thumb-rom-') as tmp:
                for index,name in enumerate(names):
                    local=Path(tmp)/'rom.32x'
                    command(['sdpull','/roms/32x/'+name,str(local)],f'rom-hash-{index}.log',180)
                    identities[name]=rig.digest(local)
                    if identities[name]==expected:matches.append(name)
            assert matches,'No SD Doom ROM matches the validated rig ROM; no flash'
            selected=matches[0];remote='/roms/32x/'+selected
            r['workload'].update(rom=remote,rom_sha256=expected)
            r['rom_candidates_sha256']=identities
            files={'bank2':'intflash-bank2.bin','/cores/32x.bin':'32x.bin','/cores/32x.xip':'32x.xip',
                   '/CONFIG':'CONFIG','/data/off.sav':'off.sav'}
            protected=('snes_bench_index.txt','snes_diag.txt','snes_dwt.txt','32x_diag.txt','32x_dwt.txt','segacd_diag.txt')
            for n in protected:
                if n in initial:files['/'+n]=n
            backup=evidence/'backup';backup.mkdir()
            args=['dump','bank2','--dst',str(backup/'intflash-bank2.bin'),'--size','262144']
            for path,n in files.items():
                if path!='bank2':args+=['--','sdpull',path,str(backup/n)]
            command(args,'backup.log',300)
            assert all((backup/n).is_file() and (backup/n).stat().st_size for n in files.values())
            original={n:rig.digest(backup/n) for n in files.values()};rig.save(backup/'manifest.json',original)
            r['backup_sha256']=original
            config=(backup/'CONFIG').read_bytes()
            assert len(config)==556 and struct.unpack_from('<I',config)[0]==0xcafef00d
            # odroid_settings.c hashes the WHOLE structure after zeroing its
            # crc32 field, including that final four-byte zero field.
            assert zlib.crc32(config[:552]+bytes(4))==struct.unpack_from('<I',config,552)[0]
            assert config[7]==0,'Persisted volume differs from prepared muted arms; no flash/start'
            selector=evidence/'selector.txt';selector.write_text('32x:'+Path(selected).stem+'\n')
            command(['sdpush','--file',str(selector),'--dest-path','/snes_bench_index.txt','--','sdpull',
                     '/snes_bench_index.txt',str(evidence/'selector-readback.txt')],'selector.log',120)
            assert selector.read_bytes()==(evidence/'selector-readback.txt').read_bytes()
            conditions=dict(workload=r['workload'],arms=manifests,
                            method=dict(rounds=rounds,max_drift_percent=0.1,sequence='A/B/A; additional triplet only if authorized and first wins beyond drift; restore on valid completion',
                                        failure_policy='stop on error; no automatic reset/retry/failure restoration'),
                            environment=dict(device_preflight='verified',live_collector_validated=True,profiling=False,
                                             collector_contract='Existing validated volume-0 completion-event collector',original_files_sha256=original))
            rig.save(evidence/'conditions.json',conditions)
            r['case_id']=hashlib.sha256(json.dumps(conditions,sort_keys=True).encode()).hexdigest()
            state('hardware_running','Doom A/B/A owns device lock; backup complete; no progress polling')
            for round_no in range(rounds):
                triplet=evidence/f'round-{round_no+1:06d}';triplet.mkdir();reports=[]
                for name,arm in (('base-before','baseline'),('candidate','candidate'),('base-after','baseline')):
                    run_arm.run(arms[arm],triplet/name,300,900,600,smoke=(round_no==0 and name=='base-before'))
                    reports.append(compare.load(triplet/name/'capture/report.json'))
                c=compare.compare(*reports,0.1)
                c.update(drawn=[p['delta_drawn'] for p in reports],emulated=[p['delta_emu'] for p in reports],
                         clock_hz=reports[0]['clock_hz'],muted_calls=[p['delta_muted_calls'] for p in reports],
                         audio_dma_stopped=[p['audio_dma_stopped'] for p in reports])
                rig.save(triplet/'comparison.json',c);r['comparisons'].append(c)
                if not(c['candidate_faster_than_both'] and c['candidate_exceeds_drift']):break
            restored=evidence/'restore';restored.mkdir()
            args=['flash','bank2',str(backup/'intflash-bank2.bin')]
            for path,n in files.items():
                if path!='bank2':args+=['--','sdpush','--file',str(backup/n),'--dest-path',path]
            for path,n in files.items():
                if path!='bank2':args+=['--','sdpull',path,str(restored/n)]
            args+=['--','dump','bank2','--dst',str(restored/'intflash-bank2.bin'),'--size','262144']
            command(args,'restore.log',300)
            checks={n:rig.digest(restored/n)==original[n] for n in files.values()}
            assert all(checks.values()),'Restoration readback differs; no restart'
            final=listing('/','root-after-bench.log');created=[n for n in protected if n not in initial and n in final]
            if created:
                args=[]
                for n in created:
                    command(['sdpull','/'+n,str(restored/('generated-'+n))],'archive-'+n+'.log',120)
                    args+=([] if not args else ['--'])+['sdrm','/'+n]
                command(args,'remove-generated.log',120)
            final=listing('/','root-restored.log')
            assert all((n in final)==(n in initial) for n in protected),'Experiment file presence differs'
            rig.save(restored/'verification.json',dict(files=checks,experiment_files_restored=True,generated_archived_and_removed=created))
            args=['openocd','-f','interface/stlink-dap.cfg','-f','target/stm32h7x.cfg','-c','adapter speed 4000',
                  '-c','init','-c','reset run','-c','sleep 1500','-c','mdw 0xe000ed08']
            if keep_awake:
                # This campaign has verified the normal firmware setter bytes
                # and live layout in device-preflight-000003. RAM-only setting
                # preserves the user's request after restoring normal firmware.
                original_bank=(backup/'intflash-bank2.bin').read_bytes()
                setter=bytes.fromhex('014ba3f810017047182e0020')
                assert original_bank.count(setter)==1, 'Normal timeout setter identity differs'
                args+=['-c','set config_magic [lindex [read_memory 0x20002e18 32 1] 0]',
                       '-c','set config_version [lindex [read_memory 0x20002e1c 8 1] 0]',
                       '-c','if {$config_magic != 0xcafef00d || $config_version != 18} {error "Normal config identity mismatch; no keep-awake write"}',
                       '-c','mwh 0x20002f28 0',
                       '-c','if {[lindex [read_memory 0x20002f28 16 1] 0] != 0} {error "Keep-awake readback differs"}',
                       '-c','echo NORMAL_KEEP_AWAKE_APPLIED']
            args+=['-c','shutdown']
            with (evidence/'normal-boot.log').open('w') as log:
                subprocess.run(args,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=30)
            assert re.search(r'0xe000ed08:\s+08100000',(evidence/'normal-boot.log').read_text(),re.I)
            if keep_awake:
                assert 'NORMAL_KEEP_AWAKE_APPLIED' in (evidence/'normal-boot.log').read_text()
                r['normal_runtime_idle_timeout_seconds']=0
            r.update(status='completed',restored=True,restoration_checks=checks,normal_boot_verified=True)
            state('hardware_completed','Original normal firmware/core/config/save/selector restored and verified; ordinary boot reached bank2 VTOR')
    except Exception as e:
        r.update(status='failed',error=str(e));(evidence/'error.log').write_text(traceback.format_exc())
        state('hardware_failed','Stopped at first error; no automatic reset/retry/recovery: '+str(e))
    rig.save(evidence/'result.json',r)
    print(json.dumps(r,ensure_ascii=False),flush=True)
    if r['status']!='completed':raise SystemExit(2)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--attempt',type=int,default=1);a=p.parse_args()
    if a.attempt<1:p.error('attempt must be positive')
    execute(a.output.resolve(),a.attempt)
