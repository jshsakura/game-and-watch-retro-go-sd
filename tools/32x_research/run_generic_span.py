#!/usr/bin/env python3
"""Finite build/run for one generic SH2 candidate; no polling or device access.

Build is separate from runtime; run waits once for QEMU process exit, then saves
results. ROM inputs remain at their original paths. No title-specific core.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import time

ROOT=Path(__file__).resolve().parents[2]
def save(path,value):path.write_text(json.dumps(value,indent=2)+'\n')
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def build(out):
    src=out/'baseline';shutil.copytree(ROOT/'external/picodrive',src,ignore=shutil.ignore_patterns('.git','*.o','*.a'))
    candidate=out/'picodrive'
    # Refresh the single header in the prepared candidate and remove proof-only
    # exports before the game compile. Source identity is recorded after this.
    path=candidate/'cpu/sh2/mame/sh2pico.c';text=path.read_text()
    start=text.index('#ifdef GNW_GENERIC_ALU_SPAN\n')
    end=text.index('\n#endif\nint sh2_execute_interpreter',start)
    text=text[:start]+'#ifdef GNW_GENERIC_ALU_SPAN\n'+(ROOT/'tools/32x_research/generic_alu_span.h').read_text()+text[end:]
    text=text.split('\n#ifdef GENERIC_SPAN_PROOF\n')[0]+'\n';path.write_text(text)
    rig=out/'rig';shutil.copytree(ROOT/'tools/m7_qemu_rig',rig,ignore=shutil.ignore_patterns('build','results','*.o','*.elf'))
    path=rig/'rig_32x.c';text=path.read_text()
    text=text.replace('        PicoIn.pad[0] = pad_script(f);','''#ifdef GENERIC_AB_INPUT
        PicoIn.pad[0] = f<120 && f%12<6 ? PAD_START : 0;
#else
        PicoIn.pad[0] = ((f>=600 && f<608) || (f>=720 && f<728) || (f>=840 && f<848)) ? PAD_B : 0;
#endif''')
    # Existing RLE dumps confirm scenes; both arms draw every frame.
    text=text.replace('if (f == RIG_FB_DUMP) fb_dump_rle(f);','if (f == RIG_WARMUP-1 || f == RIG_FRAMES-1) fb_dump_rle(f);')
    path.write_text(text)
    script=(ROOT/'tools/m7_qemu_rig/run_32x.sh').read_text()
    script=script.replace('cd "$(dirname "$0")/../.."','cd /opt/workdir')
    script=script.replace('RIG=tools/m7_qemu_rig','RIG='+str(rig.relative_to(ROOT)))
    script=script.replace('OPT="-O2 ','OPT="-O2 -Wno-error=incompatible-pointer-types ')
    # The standard rig embeds its ROM using a copy named rom.32x. Keep that
    # existing symbol name, then remove the temporary copy after objcopy.
    script=script.replace('rom.32x rom.o)','rom.32x rom.o)\nrm "$OUT/rom.32x"')
    (out/'build.sh').write_text(script)
    sources={name:{str(p.relative_to(source)):digest(p) for p in source.rglob('*') if p.suffix in ('.c','.h','.S')}
             for name,source in [('base',src),('candidate',candidate)]}
    save(out/'sources.json',sources)
    cases=[('after-burner',ROOT/'build/32x-ab-verified-entry-trace-20260930/input.32x',720,540),
           ('doom',ROOT/'build/32x-doom-native-baseline-20260930/input.32x',1500,900)]
    for title,rom,frames,warmup in cases:
        for arm,source in [('base',src),('candidate',candidate)]:
            case=out/f'{title}-{arm}';case.mkdir()
            defs=f'-DRIG_WARMUP={warmup} -DRIG_FB_DUMP=1'
            if title=='after-burner':defs+=' -DGENERIC_AB_INPUT'
            if arm=='candidate':defs+=' -DGNW_GENERIC_ALU_SPAN'
            command=['docker','run','--rm','--init','--user','1000:1000','-v',str(ROOT)+':/opt/workdir',
                '-e','PATH=/opt/arm-gnu-toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
                '-e','PD_DIR='+str(source.relative_to(ROOT)),'-e','RIG_OUT='+str(case.relative_to(ROOT)),
                '-e','RIG_BUILD_ONLY=1','-e','EXTRA_DEF='+defs,'sylverb/retro-go-sd-builder:v1.5',
                'bash',str((out/'build.sh').relative_to(ROOT)),str(rom.relative_to(ROOT)),str(frames)]
            with (case/'build.log').open('w') as log:
                subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=180)
            save(case/'inputs.json',{'title':title,'arm':arm,'rom':str(rom),'rom_sha256':digest(rom),
                'window':[warmup,frames],'frameskip':False,'profiler':False,'defs':defs,
                'input':'START 6on/6off f0..119' if title=='after-burner' else 'B f600..607,720..727,840..847',
                'elf_sha256':digest(case/'rig_32x.elf'),'device_access':False,'build_command':command})
            print('BUILT '+case.name,flush=True)
    save(out/'state.json',{'objective':'One generic 32X optimization attempt','status':'built',
        'hypothesis':'Pure arithmetic spans, no game-specific core or code cache',
        'cases':[f'{t}-{a}' for t,_,_,_ in cases for a in ('base','candidate')],
        'device_state':'untouched','next':'Compare full-game M7 instruction cost and output signatures'})

def run(case,timeout):
    manifest=json.loads((case/'inputs.json').read_text())
    assert digest(Path(manifest['rom']))==manifest['rom_sha256']
    assert digest(case/'rig_32x.elf')==manifest['elf_sha256']
    started=time.monotonic()
    command=['qemu-system-arm','-machine','mps2-an500','-nographic','-semihosting',
        '-icount','shift=0,align=off,sleep=off','-kernel',str(case/'rig_32x.elf')]
    try:
        with (case/'qemu.log').open('w') as log:
            subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=timeout)
        text=(case/'qemu.log').read_text()
        meter=re.search(r'done (\d+) frames  avg host=(\d+)  min=(\d+)  max=(\d+) insn/frame  avg sh2=(\d+)',text)
        if not meter or int(meter[1])!=manifest['window'][1] or 'GATE3 PASS' not in text:
            raise ValueError('Incomplete workload/framebuffer gate')
        result={'status':'completed','window':manifest['window'],
            'meter':dict(zip(['frames','host_insns_per_frame','min','max','sh2_instructions_per_frame'],map(int,meter.groups()))),
            'measured_frames':manifest['window'][1]-manifest['window'][0],
            'signatures':[line for line in text.splitlines() if any(k in line for k in ('snd hash=','snd energy','fb f'))],
            'elapsed_seconds':time.monotonic()-started,'device_access':False,'physical_fps_measured':False}
    except Exception as error:
        result={'status':'failed','error':str(error),'elapsed_seconds':time.monotonic()-started}
        save(case/'result.json',result);raise
    save(case/'result.json',result);print(json.dumps({'case':case.name,**result}),flush=True)

def rebuild_candidates(out):
    """Recompile the changed CPU only; preserve all matching input objects."""
    container=['docker','run','--rm','--init','--user','1000:1000','-v',str(ROOT)+':/opt/workdir','sylverb/retro-go-sd-builder:v1.5']
    arch=['-mcpu=cortex-m7','-mthumb','-mfloat-abi=hard','-mfpu=fpv5-d16','-ffp-contract=off']
    cc=['/opt/arm-gnu-toolchain/bin/arm-none-eabi-gcc',*arch,'-O2','-g','-fno-strict-aliasing','-ffunction-sections','-fdata-sections','-fcommon','-Wno-error=incompatible-pointer-types']
    pd=out/'picodrive'
    for title,frames,warmup in [('after-burner',720,540),('doom',1500,900)]:
        case=out/(title+'-candidate')
        flags=['-DGNW_32X_CORE','-DEMU_G68K','-DTABLES_FULL','-D_USE_CZ80','-DNDEBUG','-DRIG_SH2_COUNT',f'-DRIG_FRAMES={frames}',f'-DRIG_WARMUP={warmup}','-DRIG_FB_DUMP=1','-DGNW_GENERIC_ALU_SPAN']
        if title=='after-burner':flags+=['-DGENERIC_AB_INPUT']
        cpu=case/'cpu_sh2_mame_sh2pico.o'
        compile_cmd=container+cc+flags+['-I'+str(pd.relative_to(ROOT)),'-c',str((pd/'cpu/sh2/mame/sh2pico.c').relative_to(ROOT)),'-o',str(cpu.relative_to(ROOT))]
        with (case/'rebuild.log').open('w') as log:
            subprocess.run(compile_cmd,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
            objs=list(case.glob('*.o'));assert cpu in objs
            link_cmd=container+cc+['-T'+str((out/'rig/mps2_an500_32x.ld').relative_to(ROOT)),'-nostartfiles','-Wl,--gc-sections',*[str(p.relative_to(ROOT)) for p in objs],'-lm','-lc','-o',str((case/'rig_32x.elf').relative_to(ROOT))]
            subprocess.run(link_cmd,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
        manifest=json.loads((case/'inputs.json').read_text())
        manifest.update(revision=2,reused_objects_from=str(out/(title+'-candidate-v1')),rebuild_commands=[compile_cmd,link_cmd],elf_sha256=digest(case/'rig_32x.elf'))
        save(case/'inputs.json',manifest)
        print('REBUILT '+case.name,flush=True)

def proof(out):
    container=['docker','run','--rm','--init','--user','1000:1000','-v',str(ROOT)+':/opt/workdir','sylverb/retro-go-sd-builder:v1.5']
    arch=['-mcpu=cortex-m7','-mthumb','-mfloat-abi=hard','-mfpu=fpv5-d16','-ffp-contract=off']
    cc=['/opt/arm-gnu-toolchain/bin/arm-none-eabi-gcc',*arch,'-O2','-g','-fno-strict-aliasing','-ffunction-sections','-fdata-sections','-DGNW_32X_CORE','-DEMU_G68K','-DTABLES_FULL','-D_USE_CZ80']
    (out/'proof_candidate.c').write_text('#include <cpu/sh2/mame/sh2pico.c>\nunsigned test_span(SH2 *s,unsigned op){return generic_alu_span(s,op);}\nint test_op(SH2 *s,unsigned op){return generic_alu_op(s,op);}\n')
    with (out/'proof-build-final.log').open('w') as log:
        for arm,source in [('reference',out/'baseline'),('candidate',out/'picodrive')]:
            flags=[f'-Dsh2_execute_interpreter=sh2_{arm}',f'-Dgnw_sh2_fastloops={arm}_fastloops',f'-Dgnw_sh2_idle_skip={arm}_idle_skip']
            path=source/'cpu/sh2/mame/sh2pico.c'
            if arm=='candidate':
                path=out/'proof_candidate.c';flags+=['-DGNW_GENERIC_ALU_SPAN']
            subprocess.run(container+cc+flags+['-I'+str(source.relative_to(ROOT)),'-c',str(path.relative_to(ROOT)),'-o',str((out/f'proof-{arm}-final.o').relative_to(ROOT))],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
        subprocess.run(container+cc+['-Iexternal/picodrive','tools/32x_research/generic_span_proof.c',str((out/'proof-reference-final.o').relative_to(ROOT)),str((out/'proof-candidate-final.o').relative_to(ROOT)),'tools/m7_qemu_rig/rig_runtime.c','-Ttools/m7_qemu_rig/mps2_an500.ld','-nostartfiles','-Wl,--gc-sections','-lc','-lm','-o',str((out/'proof-final.elf').relative_to(ROOT))],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
    with (out/'proof-final.log').open('w') as log:
        subprocess.run(['qemu-system-arm','-machine','mps2-an500','-nographic','-semihosting','-kernel',str(out/'proof-final.elf')],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=45)
    text=(out/'proof-final.log').read_text()
    match=re.search(r'PASS generic span checks=(\d+) accepted_encodings=(\d+) declines=(\d+) context_bytes=(\d+)',text)
    if not match:raise ValueError('Differential proof did not complete')
    result={'status':'passed','checks':int(match[1]),'accepted_encodings':int(match[2]),'unchanged_declines':int(match[3]),'context_bytes':int(match[4]),'both_byte_and_word_rom_masks':True,'candidate_sha256':digest(out/'picodrive/cpu/sh2/mame/sh2pico.c'),'elf_sha256':digest(out/'proof-final.elf'),'limitations':['Synthetic full contexts and pure opcode paths; real bus/IRQ delivery also exercised by separate full-game replay.']}
    save(out/'proof-result.json',result);print(json.dumps(result),flush=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--build',action='store_true')
    parser.add_argument('--rebuild-candidates',action='store_true')
    parser.add_argument('--proof',action='store_true')
    parser.add_argument('--case')
    parser.add_argument('--timeout',type=int,default=240)
    args=parser.parse_args();out=args.output.resolve()
    if args.build:build(out)
    elif args.rebuild_candidates:rebuild_candidates(out)
    elif args.proof:proof(out)
    elif args.case:run(out/args.case,args.timeout)
    else:parser.error('Choose --build or --case')
