#!/usr/bin/env python3
"""Finite generic profile-to-native-block experiment on isolated core snapshots.

No device access. Both workloads share the same generated block module. Profiles
select blocks automatically, like a JIT; no manually chosen PC or title logic.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
LATEST = ROOT.parent / 'game-and-watch-retro-go-sd-final-20261002'
ARCH = ['-mcpu=cortex-m7', '-mthumb', '-mfloat-abi=hard', '-mfpu=fpv5-d16', '-ffp-contract=off']
FLAGS = ['-O2', '-g', '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections', '-fcommon', '-Wno-error=incompatible-pointer-types']
DEFS = ['-DGNW_32X_CORE', '-DEMU_G68K', '-DTABLES_FULL', '-D_USE_CZ80', '-DNDEBUG', '-DRIG_SH2_COUNT']
CASES = [('after-burner', ROOT/'build/32x-ab-verified-entry-trace-20260930/input.32x', 720, 540),
         ('doom', ROOT/'build/32x-doom-native-baseline-20260930/input.32x', 1500, 900)]

def save(p, v):
    p.write_text(json.dumps(v, indent=2)+'\n')

def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def container():
    return ['docker', 'run', '--rm', '--init', '--user', '1000:1000', '-v', str(ROOT)+':/opt/workdir',
            'sylverb/retro-go-sd-builder:v1.5']

def rel(p):
    return str(p.resolve().relative_to(ROOT))

def cc(args, log):
    subprocess.run(container()+['/opt/arm-gnu-toolchain/bin/arm-none-eabi-gcc', *ARCH, *FLAGS, *args],
                   cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)

def prepare(out):
    out.mkdir(parents=True, exist_ok=False)
    pd = LATEST/'external/picodrive'
    for arm in ('baseline', 'diagnostic', 'candidate'):
        shutil.copytree(pd, out/arm, ignore=shutil.ignore_patterns('.git', '*.o', '*.a'))
    save(out/'sources.json', {'worktree':str(LATEST),
         'main_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=LATEST,text=True).strip(),
         'core_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=pd,text=True).strip(),
         'core_dirty':subprocess.check_output(['git','status','--porcelain'],cwd=pd,text=True),
         'files':{str(p.relative_to(pd)):digest(p) for p in pd.rglob('*') if p.suffix in ('.c','.h','.S')}})
    rig=out/'rig'
    shutil.copytree(ROOT/'tools/m7_qemu_rig',rig,ignore=shutil.ignore_patterns('build','results','*.o','*.elf'))
    path=rig/'rig_32x.c'; text=path.read_text()
    text=text.replace('        PicoIn.pad[0] = pad_script(f);', '''#ifdef AOT_AB_INPUT
        PicoIn.pad[0] = f<120 && f%12<6 ? PAD_START : 0;
#else
        PicoIn.pad[0] = ((f>=600 && f<608) || (f>=720 && f<728) || (f>=840 && f<848)) ? PAD_B : 0;
#endif''')
    text=text.replace('if (f == RIG_FB_DUMP) fb_dump_rle(f);',
                      'if (f == RIG_WARMUP-1 || f == RIG_FRAMES-1) fb_dump_rle(f);')
    text=text.replace('int main(void)', '#ifdef AOT_TRACE\nextern void trace_report(void);\n#endif\nint main(void)')
    # Report only after the entire finite frame loop.
    marker='    return pass ? 0 : 4;'
    pos=text.rfind(marker)
    assert pos>0
    text=text[:pos]+'''#ifdef AOT_TRACE
    trace_report();
#endif
'''+text[pos:]
    path.write_text(text)
    path=out/'diagnostic/cpu/sh2/mame/sh2pico.c';text=path.read_text()
    anchor='int sh2_execute_interpreter(SH2 *sh2, int cycles)\n{'
    assert text.count(anchor)==2
    trace=(ROOT/'tools/32x_research/guest_trace.h').read_text()
    # The two recorded gameplay workloads require >8192 distinct runs.
    # Keep the known zero-loss table inside mapped RAM; a larger BIG-section
    # table pushes the rig stack past the board's mapped PSRAM.
    trace=trace.replace('#define TRACE_SLOTS 8192','#define TRACE_SLOTS 16384')
    trace=trace.replace('i<64;','i<256;')
    text=text.replace(anchor,trace+'\n'+anchor,1)
    text=text.replace('\tsh2->icount = cycles;', '\ttrace_flush(sh2->is_slave&1);\n\tsh2->icount = cycles;',1)
    text=text.replace('\t\tRIG_SH2_TICK();','\t\ttrace_tick(sh2, opcode, rig_is_delay);\n\t\tRIG_SH2_TICK();',1)
    text=text.replace('out:\n\treturn sh2->icount;', 'out:\n\ttrace_flush(sh2->is_slave&1);\n\treturn sh2->icount;',1)
    path.write_text(text)
    script=(ROOT/'tools/m7_qemu_rig/run_32x.sh').read_text()
    script=script.replace('cd "$(dirname "$0")/../.."','cd /opt/workdir').replace('RIG=tools/m7_qemu_rig','RIG='+rel(rig))
    script=script.replace('OPT="-O2 ','OPT="-O2 -Wno-error=incompatible-pointer-types ')
    script=script.replace('rom.32x rom.o)','rom.32x rom.o)\nrm "$OUT/rom.32x"')
    (out/'build.sh').write_text(script)
    save(out/'state.json',{'status':'prepared','objective':'Generic automatic SH2 native blocks with code guards and direct safe reads',
         'device_state':'untouched','next':'Build current baseline and zero-loss diagnostic, then generate shared module'})

def definitions(title, frames, warmup, trace=False):
    v=[*DEFS,f'-DRIG_FRAMES={frames}',f'-DRIG_WARMUP={warmup}','-DRIG_FB_DUMP=1']
    if title=='after-burner':v+=['-DAOT_AB_INPUT']
    if trace:v+=['-DAOT_TRACE','-DRIG_SH2_PC_HIST',f'-DTRACE_FROM={warmup}',f'-DTRACE_TO={frames}']
    return v

def build_initial(out):
    for title,rom,frames,warmup in CASES:
        for arm in ('baseline','diagnostic'):
            case=out/f'{title}-{arm}';case.mkdir()
            defs=definitions(title,frames,warmup,arm=='diagnostic')
            cmd=['docker','run','--rm','--init','--user','1000:1000','-v',str(ROOT)+':/opt/workdir',
                 '-e','PATH=/opt/arm-gnu-toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
                 '-e','PD_DIR='+rel(out/arm),'-e','RIG_OUT='+rel(case),'-e','RIG_BUILD_ONLY=1',
                 '-e','EXTRA_DEF='+' '.join(defs[6:]),'sylverb/retro-go-sd-builder:v1.5',
                 'bash',rel(out/'build.sh'),rel(rom),str(frames)]
            # Script already defines the base flags; pass extra flags only.
            with (case/'build.log').open('w') as log:
                subprocess.run(cmd,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=240)
            save(case/'inputs.json',{'rom':str(rom),'rom_sha256':digest(rom),'window':[warmup,frames],
                 'defs':defs,'build_command':cmd,'elf_sha256':digest(case/'rig_32x.elf'),'diagnostic':arm=='diagnostic',
                 'input':'START6/6 f0..119' if title=='after-burner' else 'B f600..607,720..727,840..847'})
            print('BUILT '+case.name,flush=True)

def run(case, timeout=300):
    meta=json.loads((case/'inputs.json').read_text())
    assert digest(Path(meta['rom']))==meta['rom_sha256'] and digest(case/'rig_32x.elf')==meta['elf_sha256']
    cmd=['qemu-system-arm','-machine','mps2-an500','-nographic','-semihosting']
    if not meta['diagnostic']:cmd+=['-icount','shift=0,align=off,sleep=off']
    cmd+=['-kernel',str(case/'rig_32x.elf')]
    with (case/'qemu.log').open('w') as log:
        subprocess.run(cmd,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=timeout)
    text=(case/'qemu.log').read_text()
    meter=re.search(r'done (\d+) frames  avg host=(\d+)  min=(\d+)  max=(\d+) insn/frame  avg sh2=(\d+)',text)
    if not meter or int(meter[1])!=meta['window'][1] or 'GATE3 PASS' not in text:raise ValueError('Incomplete run')
    result={'status':'completed','window':meta['window'],'diagnostic':meta['diagnostic'],
            'host_insns_per_frame':None if meta['diagnostic'] else int(meter[2]),
            'guest_dispatches_per_frame':int(meter[5]),
            'signatures':[s for s in text.splitlines() if any(k in s for k in ('snd hash=','snd energy','fb f'))]}
    if meta['diagnostic']:
        runs=[]
        for line in text.splitlines():
            if line.startswith('TRACE_RUN '):
                v=line.split()[1:];runs.append({'core':int(v[0]),'pc':int(v[1],16),'delay':int(v[2]),
                                              'count':int(v[3]),'ops':[int(x,16) for x in v[4:]]})
        dropped=re.search(r'^TRACE_DROPPED (\d+)$',text,re.M)
        totals=re.findall(r'^TRACE_CORE \d (\d+) ',text,re.M)
        if 'TRACE_END' not in text or len(totals)!=2 or not dropped or int(dropped[1]):raise ValueError('Trace lost operations')
        total=sum(map(int,totals))
        assert sum(len(r['ops'])*r['count'] for r in runs)==total
        result.update(trace_dropped=0,observed_dispatches=total,runs=len(runs))
        save(case/'runs.json',runs)
    save(case/'result.json',result)
    print(json.dumps({'case':case.name,**result}),flush=True)

def compare(out, suffix='candidate'):
    """Require identical complete snapshots/audio/progress before cost reporting."""
    results=[]
    for title,_,_,_ in CASES:
        base=out/f'{title}-baseline';candidate=out/f'{title}-{suffix}'
        b=json.loads((base/'result.json').read_text())
        c=json.loads((candidate/'result.json').read_text())
        dumps=[re.findall(r'\[fbd\] frame=.*?\[fbd\] end',
                         (p/'qemu.log').read_text(),re.S) for p in (base,candidate)]
        assert b['window']==c['window'] and b['signatures']==c['signatures']
        assert b['guest_dispatches_per_frame']==c['guest_dispatches_per_frame']
        assert len(dumps[0])==2 and dumps[0]==dumps[1]
        results.append(dict(title=title,window=b['window'],
            baseline_host_insns_per_frame=b['host_insns_per_frame'],
            candidate_host_insns_per_frame=c['host_insns_per_frame'],
            cost_change_percent=100*(c['host_insns_per_frame']/b['host_insns_per_frame']-1),
            full_framebuffers_equal=True,audio_signatures_equal=True,guest_dispatches_equal=True,
            framebuffers_sha256=[hashlib.sha256(d.encode()).hexdigest() for d in dumps[0]],
            baseline_elf_sha256=digest(base/'rig_32x.elf'),candidate_elf_sha256=digest(candidate/'rig_32x.elf')))
    save(out/f'comparison-{suffix}.json',results)
    print(json.dumps(results),flush=True)
    return results

def build_candidate(out, suffix='candidate', extra=()):
    for title,rom,frames,warmup in CASES:
        case=out/f'{title}-{suffix}';case.mkdir()
        base=out/f'{title}-baseline'
        for obj in base.glob('*.o'):shutil.copyfile(obj,case/obj.name)
        defs=definitions(title,frames,warmup)+list(extra)
        with (case/'build.log').open('w') as log:
            cc([*defs,'-I'+rel(out/'candidate'),'-I'+rel(out),'-c',rel(out/'candidate/cpu/sh2/mame/sh2pico.c'),'-o',rel(case/'cpu_sh2_mame_sh2pico.o')],log)
            cc([*defs,'-I'+rel(out/'candidate'),'-I'+rel(out),'-c',rel(out/'native_module.c'),'-o',rel(case/'native_module.o')],log)
            cc(['-T'+rel(out/'rig/mps2_an500_32x.ld'),'-nostartfiles','-Wl,--gc-sections','-Wl,-Map='+rel(case/'rig.map'),
                *[rel(p) for p in case.glob('*.o')],'-lm','-lc','-o',rel(case/'rig_32x.elf')],log)
        meta=json.loads((base/'inputs.json').read_text());meta.update(defs=defs,elf_sha256=digest(case/'rig_32x.elf'),
              module_sha256=digest(out/'native_module.c'),guarded_code=True,extra=list(extra),diagnostic=False)
        save(case/'inputs.json',meta)
        print('BUILT '+case.name,flush=True)

def proof(out):
    with (out/'proof-build.log').open('w') as log:
        cc([*DEFS,'-Dsh2_execute_interpreter=sh2_aot_reference','-I'+rel(out/'baseline'),
            '-c',rel(out/'baseline/cpu/sh2/mame/sh2pico.c'),'-o',rel(out/'proof_reference.o')],log)
        cc([*DEFS,'-I'+rel(out/'baseline'),'-I'+rel(out),
            'tools/32x_research/aot_proof.c',rel(out/'native_module.c'),rel(out/'proof_reference.o'),
            'tools/m7_qemu_rig/rig_runtime.c','-T'+rel(out/'rig/mps2_an500_32x.ld'),'-nostartfiles',
            '-Wl,--gc-sections','-lc','-lm','-o',rel(out/'proof.elf')],log)
    with (out/'proof.log').open('w') as log:
        subprocess.run(['qemu-system-arm','-machine','mps2-an500','-nographic','-semihosting','-kernel',str(out/'proof.elf')],
                       cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
    text=(out/'proof.log').read_text()
    match=re.search(r'PASS aot blocks=(\d+) checks=(\d+) full=(\d+) partial=(\d+) declines=(\d+) context_bytes=(\d+)',text)
    if not match:raise ValueError('Proof did not complete')
    save(out/'proof-result.json',dict(zip(['blocks','checks','full','partial','declines','context_bytes'],map(int,match.groups()))))
    print(text.strip(),flush=True)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--stage',required=True,choices=['prepare','initial','diagnostic','baseline','candidate','proof','compare'])
    parser.add_argument('--suffix',default='candidate')
    args=parser.parse_args();out=args.output.resolve()
    if args.stage=='prepare':prepare(out)
    elif args.stage=='initial':build_initial(out)
    elif args.stage in ('diagnostic','baseline'):
        for title,_,_,_ in CASES:run(out/f'{title}-{args.stage}')
    elif args.stage=='candidate':
        build_candidate(out,args.suffix)
        for title,_,_,_ in CASES:run(out/f'{title}-{args.suffix}')
        compare(out,args.suffix)
    elif args.stage=='proof':proof(out)
    elif args.stage=='compare':compare(out,args.suffix)

if __name__=='__main__':main()
