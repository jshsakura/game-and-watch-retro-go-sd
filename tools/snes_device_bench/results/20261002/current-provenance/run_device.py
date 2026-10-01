#!/usr/bin/env python3
"""Own the probe for a finite smoke and A/B/A; stop at the first execution error."""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import struct
import zlib
from collect import collect

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()

def command(output, label, args):
    with (output/(label+'.log')).open('w') as log:
        subprocess.run([sys.executable,str(Path(__file__).with_name('gnw_command.py')),*args],
            stdout=log,stderr=subprocess.STDOUT,timeout=180,check=True)

def run_arm(arm, backup, output, smoke, expected):
    manifest=json.loads((arm/'manifest.json').read_text())
    for name,digest in manifest['sha256'].items():
        if sha(arm/name)!=digest: raise ValueError('changed arm '+name)
    if manifest['profiler'] or manifest['warmup']!=120 or manifest['frames']!=900:
        raise ValueError('unexpected instrumentation or window')
    output.mkdir(parents=True,exist_ok=False)
    v=output/'verification';v.mkdir()
    args=['flash','bank2',str(arm/'gw_retro_go_intflash.bin'),
        '--','sdpush','--file',str(arm/'snes.bin'),'--dest-path','/cores/snes.bin',
        '--','sdpush','--file',expected.get('config_file',str(backup/'CONFIG')),'--dest-path','/CONFIG',
        '--','sdpush','--file',str(output.parent/'selector.txt'),'--dest-path','/snes_bench_index.txt',
        '--','sdpull','/cores/snes.bin',str(v/'snes.bin'),
        '--','sdpull','/CONFIG',str(v/'CONFIG'),
        '--','sdpull','/snes_bench_index.txt',str(v/'selector.txt'),
        '--','dump','bank2','--dst',str(v/'gw_retro_go_intflash.bin'),
        '--size',str((arm/'gw_retro_go_intflash.bin').stat().st_size)]
    command(output,'flash',args)
    for name in ['snes.bin','gw_retro_go_intflash.bin']:
        if sha(v/name)!=manifest['sha256'][name]: raise ValueError('arm readback '+name)
    if sha(v/'CONFIG')!=expected['config_sha256'] or (v/'selector.txt').read_bytes()!=expected['selector'].encode():
        raise ValueError('workload readback mismatch')
    command(output,'start',['start','bank2'])
    r=collect(arm/'gw_retro_go.elf',output/'capture',60,3333,120,smoke,force_draw=expected.get('force_draw',0))
    if not r['audio_dma_stopped']: raise ValueError('SAI/DMA still enabled at completion')
    if r['rom_crc32']!=expected['rom_crc32'] or r['rom_bytes']!=expected['rom_bytes']:
        raise ValueError('autoboot selected another ROM')
    if r['initial_state_crc32']!=expected['save_payload_crc32']:
        raise ValueError('loaded state differs from backed-up payload')
    if r['last_drawn_frame']!=1019: raise ValueError('wrong captured endpoint')
    if r['gapfree']!=1 or r['speedup']!=0: raise ValueError('unexpected runtime audio/speed mode')
    if expected.get('cpu_oc') == 2 and r['clock_hz'] != 340000000:
        raise ValueError('requested menu clock not actually applied')
    (output/'result.json').write_text(json.dumps(dict(status='completed',manifest=manifest,capture=r),indent=2)+'\n')
    return r

def compare(records, path):
    a,b,c=records
    identity=['rom_crc32','rom_bytes','state_resumed','initial_state_crc32',
              'initial_guest_frame','start_guest_frame','end_guest_frame',
              'clock_hz','last_drawn_frame','gapfree','speedup','audio_buffer_samples']
    mismatch=[k for k in identity if len({r[k] for r in records})!=1]
    if mismatch: raise ValueError('different workloads: '+','.join(mismatch))
    drift=(c['drawn_fps']/a['drawn_fps']-1)*100
    avg=(a['drawn_fps']+c['drawn_fps'])/2
    r=dict(status='descriptive_only',base_before=a['drawn_fps'],candidate=b['drawn_fps'],
        base_after=c['drawn_fps'],base_mean=avg,gain_percent=(b['drawn_fps']/avg-1)*100,
        base_drift_percent=drift,drift_limit_percent=0.5,drift_ok=abs(drift)<=0.5,
        guest_state_crc32_equal=len({x['end_state_crc32'] for x in records})==1,
        endpoint_screen_equal=len({x['screen_sha256'] for x in records})==1,
        endpoint_pcm_equal=len({x['audio_sha256'] for x in records})==1,
        audio_underruns=[x['delta_underruns'] for x in records],
        limitations=['one fixed triplet; endpoint CRC32/screens/PCM are not full-stream hardware validation',
                     'timing includes finite observation hook and one forced drawn endpoint'])
    if not r['drift_ok']: r['status']='invalid_drift'
    if not r['guest_state_crc32_equal'] or not r['endpoint_screen_equal'] or not r['endpoint_pcm_equal']:
        r['status']='correctness_unresolved'
    path.write_text(json.dumps(r,indent=2)+'\n')
    return r

def restore(backup, output, save_path, save_file):
    output.mkdir(exist_ok=False)
    args=['flash','bank2',str(backup/'intflash-bank2.bin')]
    for local,remote in [('snes.bin','/cores/snes.bin'),('CONFIG','/CONFIG'),('selector.txt','/snes_bench_index.txt')]:
        args+=['--','sdpush','--file',str(backup/local),'--dest-path',remote]
        args+=['--','sdpull',remote,str(output/local)]
    args+=['--','dump','bank2','--dst',str(output/'intflash-bank2.bin'),'--size','262144',
           '--','sdpull',save_path,str(output/save_file),
           '--','sdpull','/data/off.sav',str(output/'off.sav')]
    command(output,'restore',args)
    verified={n:sha(output/n)==sha(backup/n) for n in
              ['snes.bin','CONFIG','selector.txt','intflash-bank2.bin',save_file,'off.sav']}
    (output/'readback.json').write_text(json.dumps(verified,indent=2)+'\n')
    if not all(verified.values()): raise ValueError('original restoration mismatch')
    command(output,'start',['start','bank2'])

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pair',type=Path,required=True)
    p.add_argument('--backup',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--save-path',required=True)
    p.add_argument('--save-file',default='zelda-slot0.sav')
    p.add_argument('--rom-file',default='zelda.smc')
    p.add_argument('--selector',default='0')
    p.add_argument('--validated-smoke',type=Path)
    p.add_argument('--rerun-reason')
    p.add_argument('--force-draw',action='store_true',help='diagnostic only: render all measured frames in both arms')
    p.add_argument('--cpu-oc',type=int,choices=[0,1,2],help='temporary supported menu setting; original CONFIG restored')
    a=p.parse_args()
    a.pair=a.pair.resolve(strict=True);a.backup=a.backup.resolve(strict=True)
    a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=False)
    with open('/tmp/gnw-swd.lock','a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        rom=(a.backup/a.rom_file).read_bytes()
        expected=dict(rom_crc32=zlib.crc32(rom),rom_bytes=len(rom),rom_sha256=sha(a.backup/a.rom_file),
            config_sha256=sha(a.backup/'CONFIG'),save_sha256=sha(a.backup/a.save_file),
            save_payload_crc32=zlib.crc32((a.backup/a.save_file).read_bytes()[12:]),selector=a.selector+'\n',force_draw=int(a.force_draw))
        if a.cpu_oc is not None:
            config=bytearray((a.backup/'CONFIG').read_bytes())
            if len(config)!=556 or config[:5]!=bytes.fromhex('0df0feca12'):
                raise ValueError('unsupported CONFIG layout for clock experiment')
            original_crc=struct.unpack('<I',config[-4:])[0]
            config[-4:]=bytes(4)
            if zlib.crc32(config)!=original_crc: raise ValueError('bad original CONFIG CRC')
            config[15]=a.cpu_oc
            config[-4:]=struct.pack('<I',zlib.crc32(config))
            bench_config=a.output/'bench-CONFIG';bench_config.write_bytes(config)
            expected.update(config_file=str(bench_config),config_sha256=sha(bench_config),cpu_oc=a.cpu_oc)
        (a.output/'selector.txt').write_bytes(expected['selector'].encode())
        (a.output/'conditions.json').write_text(json.dumps(dict(expected=expected,
            sequence=['smoke','base-before','candidate','base-after','restore'],input='no buttons',
            warmup=120,window=900,save_path=a.save_path,profiler=False,rerun_reason=a.rerun_reason),indent=2)+'\n')
        try:
            admission=a.output/'admission';admission.mkdir()
            args=['dump','bank2','--dst',str(admission/'intflash-bank2.bin'),'--size','262144']
            for local,remote in [('snes.bin','/cores/snes.bin'),('CONFIG','/CONFIG'),
                ('selector.txt','/snes_bench_index.txt'),(a.save_file,a.save_path),('off.sav','/data/off.sav')]:
                args+=['--','sdpull',remote,str(admission/local)]
            command(admission,'admission',args)
            for name in ['intflash-bank2.bin','snes.bin','CONFIG','selector.txt',a.save_file,'off.sav']:
                if sha(admission/name)!=sha(a.backup/name):
                    raise ValueError('device changed since backup: '+name+'; no firmware written')
            if a.validated_smoke:
                smoke=json.loads(a.validated_smoke.read_text())
                base=json.loads((a.pair/'A/manifest.json').read_text())
                if smoke.get('elf_sha256')!=base['sha256']['gw_retro_go.elf'] or not smoke.get('audio_dma_stopped') or not smoke.get('wall_timer_smoke'):
                    raise ValueError('smoke evidence does not validate this binary')
                (a.output/'smoke-reuse.json').write_text(json.dumps(dict(source=str(a.validated_smoke.resolve()),sha256=sha(a.validated_smoke)),indent=2)+'\n')
            else:
                smoke=run_arm(a.pair/'A',a.backup,a.output/'smoke',True,expected)
            records=[run_arm(a.pair/arm,a.backup,a.output/name,False,expected)
                for name,arm in [('base-before','A'),('candidate','B'),('base-after','A')]]
            report=compare(records,a.output/'comparison.json')
            restore(a.backup,a.output/'restore',a.save_path,a.save_file)
            (a.output/'result.json').write_text(json.dumps(dict(status='completed',comparison=report,
                smoke_timer=smoke['wall_timer_smoke'],original_restored=True),indent=2)+'\n')
            print(json.dumps(report,ensure_ascii=False))
        except Exception as e:
            (a.output/'failure.json').write_text(json.dumps(dict(status='failed',error=str(e),
                automatic_reset_or_retry=False),indent=2)+'\n')
            raise

if __name__=='__main__': main()
