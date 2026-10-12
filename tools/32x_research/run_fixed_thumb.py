#!/usr/bin/env python3
"""Finite fixed-register SH2 proof and full-game A/B, isolated from production."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import fixed_thumb as f
import run_aot_blocks as rig

ROOT=rig.ROOT
PRIOR=ROOT/'build/32x-aot-blocks-20261007'
RENAMES=['blita_report','blitb_report','blitc_report','fill_report','gnw_cmdidle_stat','gnw_copyw_stat',
         'gnw_sh2_fastloops','gnw_sh2_idle_skip','idle_report','irq_report','mix_budget_hist',
         'mix_master_bucket','mix_pcbin','mix_report','mix_slave_bucket']

def prepare(out, predecoded=False, branches=False, latest=False, writes=False, baseline_from=None):
    out.mkdir(parents=True,exist_ok=False)
    shutil.copytree(PRIOR/'baseline',out/'baseline')
    shutil.copytree(PRIOR/'baseline',out/'candidate')
    shutil.copytree(PRIOR/'rig',out/'rig')
    shutil.copyfile(PRIOR/'sources.json',out/'sources.json')
    for title,_,_,_ in rig.CASES:
        # Reuse completed arm binaries and results; no new ROM copy.
        if baseline_from:(out/f'{title}-baseline').symlink_to(baseline_from.resolve()/f'{title}-baseline')
        elif not latest:(out/f'{title}-baseline').symlink_to(PRIOR/f'{title}-baseline')
    if latest:
        source=ROOT.parent/'game-and-watch-retro-go-sd-pcm-20261005/external/picodrive'
        # Only this translation unit and its private includes changed since
        # the saved arms. All other objects and the embedded ROM are reused.
        changed=subprocess.check_output(['git','diff','--name-only','ce4ebb6cd','HEAD'],cwd=source,text=True).splitlines()
        assert set(changed)=={'cpu/sh2/mame/sh2.c','cpu/sh2/mame/sh2pico.c','cpu/sh2/mame/pcm_span.h','cpu/sh2/mame/pcm_synthetic.h'}
        assert not subprocess.check_output(['git','status','--porcelain'],cwd=source,text=True)
        for arm in ('baseline','candidate'):
            for name in changed:shutil.copyfile(source/name,out/arm/name)
        meta=json.loads((out/'sources.json').read_text())
        meta.update(core_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=source,text=True).strip(),
                    worktree=str(source.parent.parent),reused_objects_from=str(PRIOR),changed_translation_unit='cpu/sh2/mame/sh2pico.c')
        meta['files'].update({name:rig.digest(source/name) for name in changed})
        rig.save(out/'sources.json',meta)
    path=out/'candidate/cpu/sh2/mame/sh2pico.c'
    path.write_text(f.step_source(path.read_text(),predecoded,writes))
    f.emit(out/'fixed_thumb.S',predecoded,branches,writes)
    off='#include <stddef.h>\n#include <cpu/sh2/sh2.h>\nvoid offsets(void){\n'
    for field in f.OFFSETS:
        off+=f'__asm__("\\n.equ SH2_{field.upper()},%c0"::"i"(offsetof(SH2,{field})));\n'
    off+='}\n';(out/'offsets.c').write_text(off)
    with (out/'offset-build.log').open('w') as log:
        rig.cc([*rig.DEFS,'-I'+rig.rel(out/'baseline'),'-S',rig.rel(out/'offsets.c'),'-o',rig.rel(out/'offsets.s')],log)
    offsets=re.findall(r'\.equ SH2_\w+,\d+', (out/'offsets.s').read_text())
    assert len(offsets)==len(f.OFFSETS)
    (out/'sh2_offsets.inc').write_text('\n'.join(offsets)+'\n')
    allops=[op for op in range(65536) if f.supported(op,branches,writes)]
    edge=[]
    pairs={(0,0),(0,1),(1,0),(7,15),(15,7),(15,15)}
    for op in allops:
        major=op>>12;n=(op>>8)&15;m=(op>>4)&15
        if major in (7,14,9,13):include=n in (0,1,7,15) and op&255 in (0,1,127,128,255)
        elif major in (1,2,3,5,6) or (major==0 and op&15 in (4,5,6,7,12,13,14)):include=(n,m) in pairs and (major not in (1,5) or op&15 in (0,15))
        elif major==4:include=n in (0,1,7,15)
        elif major in (8,12):include=op&255 in (0,1,127,128,255)
        elif major in (10,11):include=op&4095 in (0,1,2047,2048,4093,4094,4095)
        else:include=n in (0,1,7,15)
        if include:edge.append(op)
    header=f'#define FIXED_OP_COUNT {len(allops)}\n#define FIXED_EDGE_COUNT {len(edge)}\n'
    header+='static const uint16_t fixed_ops[]={'+','.join(map(str,allops))+'};\n'
    header+='static const uint16_t fixed_edges[]={'+','.join(map(str,edge))+'};\n'
    (out/'fixed_fixtures.h').write_text(header)
    rig.save(out/'state.json',dict(status='prepared',objective='Replace raw SH2 dispatch with fixed-register Thumb execution; no ROM/profile/PC specialization',
        current_best=json.loads((out/'sources.json').read_text())['core_commit'],branches=branches,writes=writes,
        device_state='No firmware/reset/RAM changes',next='Full-context proof, then complete-game instruction-count A/B'))
    print(json.dumps(dict(status='prepared',opcodes=len(allops),edge_fixtures=len(edge))),flush=True)

def objects(out,defs,case,log):
    rig.cc([*defs,'-I'+rig.rel(out/'candidate'),'-c',rig.rel(out/'candidate/cpu/sh2/mame/sh2pico.c'),'-o',rig.rel(case/'cpu_sh2_mame_sh2pico.o')],log)
    rig.cc([*defs,'-I'+rig.rel(out),'-c',rig.rel(out/'fixed_thumb.S'),'-o',rig.rel(case/'fixed_thumb.o')],log)

def proof(out):
    case=out/'proof';case.mkdir()
    with (case/'build.log').open('w') as log:
        objects(out,rig.DEFS,case,log)
        rig.cc([*[d for d in rig.DEFS if d!='-DRIG_SH2_COUNT'],'-Dsh2_execute_interpreter=sh2_fixed_reference',
                *[f'-D{x}=reference_{x}' for x in RENAMES],'-I'+rig.rel(out/'baseline'),'-c',rig.rel(out/'baseline/cpu/sh2/mame/sh2pico.c'),'-o',rig.rel(case/'reference.o')],log)
        rig.cc([*rig.DEFS,'-I'+rig.rel(out/'baseline'),'-I'+rig.rel(out),
                'tools/32x_research/fixed_thumb_proof.c','tools/m7_qemu_rig/rig_runtime.c',
                *[rig.rel(p) for p in case.glob('*.o')],'-T'+rig.rel(out/'rig/mps2_an500_32x.ld'),'-nostartfiles','-Wl,--gc-sections',
                '-Wl,-Map='+rig.rel(case/'proof.map'),'-lm','-lc','-o',rig.rel(case/'proof.elf')],log)
    with (case/'qemu.log').open('w') as log:
        subprocess.run(['qemu-system-arm','-machine','mps2-an500','-nographic','-semihosting','-kernel',str(case/'proof.elf')],
                       stdout=log,stderr=subprocess.STDOUT,check=True,timeout=180)
    text=(case/'qemu.log').read_text();m=re.search(r'PASS fixed-thumb checks=(\d+) opcodes=(\d+) edge_fixtures=(\d+) context_bytes=(\d+) irq_calls=(\d+)',text)
    if not m:raise ValueError('Fixed-register proof did not complete')
    result=dict(zip(['checks','opcodes','edge_fixtures','context_bytes','irq_calls'],map(int,m.groups())))
    result['elf_sha256']=rig.digest(case/'proof.elf');rig.save(case/'result.json',result)
    print(text.strip(),flush=True)

def baseline(out):
    for title,_,_,_ in rig.CASES:
        case=out/f'{title}-baseline';case.mkdir()
        prior=PRIOR/case.name
        for obj in prior.glob('*.o'):shutil.copyfile(obj,case/obj.name)
        meta=json.loads((prior/'inputs.json').read_text())
        with (case/'build.log').open('w') as log:
            rig.cc([*meta['defs'],'-I'+rig.rel(out/'baseline'),'-c',rig.rel(out/'baseline/cpu/sh2/mame/sh2pico.c'),
                    '-o',rig.rel(case/'cpu_sh2_mame_sh2pico.o')],log)
            rig.cc(['-T'+rig.rel(out/'rig/mps2_an500_32x.ld'),'-nostartfiles','-Wl,--gc-sections',
                    *[rig.rel(p) for p in case.glob('*.o')],'-lm','-lc','-o',rig.rel(case/'rig_32x.elf')],log)
        meta.update(elf_sha256=rig.digest(case/'rig_32x.elf'),source_manifest=str(out/'sources.json'))
        rig.save(case/'inputs.json',meta);rig.run(case)

def clean(out,suffix):
    """Remove the observer from BOTH CPU arms. Counted runs prove guest progress;
    clean runs must still match their counted arm's entire screen and audio."""
    counted=json.loads((out/f'comparison-{suffix}.json').read_text())
    harness=out/'rig/rig_32x_clean.c'
    src=(out/'rig/rig_32x.c').read_text()
    src=src.replace('extern unsigned long long g_sh2_insns;','static unsigned long long g_sh2_insns; /* observer disabled */')
    src=src.replace('int pass = nbend && moving && sh2_tot > 0;','int pass = nbend && moving;')
    src=src.replace('fb alive and moving, SH-2s executing','fb alive and moving; guest observer disabled (validated separately)')
    harness.write_text(src)
    rows=[]
    for title,_,frames,warmup in rig.CASES:
        vals=[]
        for arm in ('baseline',suffix):
            prior=out/f'{title}-{arm}';case=out/f'{title}-{arm}-clean'
            reused=prior.resolve().with_name(prior.name+'-clean')
            if arm=='baseline' and prior.is_symlink() and (reused/'result.json').is_file():
                assert rig.digest(prior/'rig_32x.elf')==rig.digest(prior.resolve()/'rig_32x.elf')
                inputs=json.loads((reused/'inputs.json').read_text())
                assert rig.digest(reused/'rig_32x.elf')==inputs['elf_sha256'] and inputs['guest_observer'] is False
                assert inputs['rom_sha256']==rig.digest(Path(inputs['rom']))
                case.symlink_to(reused)
                vals.append(json.loads((case/'result.json').read_text())['host_insns_per_frame'])
                print('REUSED '+case.name,flush=True);continue
            case.mkdir()
            for obj in prior.glob('*.o'):shutil.copyfile(obj,case/obj.name)
            defs=[*rig.definitions(title,frames,warmup),'-URIG_SH2_COUNT']
            with (case/'build.log').open('w') as log:
                if arm=='baseline':rig.cc([*defs,'-I'+rig.rel(out/'baseline'),'-c',rig.rel(out/'baseline/cpu/sh2/mame/sh2pico.c'),'-o',rig.rel(case/'cpu_sh2_mame_sh2pico.o')],log)
                else:objects(out,defs,case,log)
                rig.cc([*defs,'-I'+rig.rel(out/'baseline'),'-I'+rig.rel(out/'baseline/pico'),'-I'+rig.rel(out/'rig'),
                        '-c',rig.rel(harness),'-o',rig.rel(case/'rig_32x.o')],log)
                rig.cc(['-T'+rig.rel(out/'rig/mps2_an500_32x.ld'),'-nostartfiles','-Wl,--gc-sections',
                        *[rig.rel(p) for p in case.glob('*.o')],'-lm','-lc','-o',rig.rel(case/'rig_32x.elf')],log)
            meta=json.loads((prior/'inputs.json').read_text());meta.update(defs=defs,elf_sha256=rig.digest(case/'rig_32x.elf'),guest_observer=False)
            rig.save(case/'inputs.json',meta);rig.run(case)
            result=json.loads((case/'result.json').read_text());ref=json.loads((prior/'result.json').read_text())
            assert result['signatures']==ref['signatures']
            snaps=lambda p:re.findall(r'\[fbd\] frame=.*?\[fbd\] end',(p/'qemu.log').read_text(),re.S)
            assert len(snaps(case))==2 and snaps(case)==snaps(prior)
            result['guest_dispatches_per_frame']=None;result['guest_observer']=False;rig.save(case/'result.json',result)
            vals.append(result['host_insns_per_frame'])
        rows.append(dict(title=title,window=[warmup,frames],baseline_host_insns_per_frame=vals[0],
                         candidate_host_insns_per_frame=vals[1],cost_change_percent=100*(vals[1]/vals[0]-1),
                         guest_observer=False,screen_audio_equal_to_counted=True,
                         counted_guest_dispatches_equal=next(x for x in counted if x['title']==title)['guest_dispatches_equal']))
    rig.save(out/f'comparison-{suffix}-clean.json',rows);print(json.dumps(rows),flush=True)

def candidate(out,suffix):
    assert (out/'proof/result.json').is_file()
    for title,_,frames,warmup in rig.CASES:
        case=out/f'{title}-{suffix}';case.mkdir()
        base=out/f'{title}-baseline'
        for obj in base.glob('*.o'):shutil.copyfile(obj,case/obj.name)
        defs=rig.definitions(title,frames,warmup)
        with (case/'build.log').open('w') as log:
            objects(out,defs,case,log)
            rig.cc(['-T'+rig.rel(out/'rig/mps2_an500_32x.ld'),'-nostartfiles','-Wl,--gc-sections','-Wl,-Map='+rig.rel(case/'rig.map'),
                    *[rig.rel(p) for p in case.glob('*.o')],'-lm','-lc','-o',rig.rel(case/'rig_32x.elf')],log)
        meta=json.loads((base/'inputs.json').read_text());meta.update(elf_sha256=rig.digest(case/'rig_32x.elf'),
            assembly_sha256=rig.digest(out/'fixed_thumb.S'),fallback_sha256=rig.digest(out/'candidate/cpu/sh2/mame/sh2pico.c'),diagnostic=False)
        rig.save(case/'inputs.json',meta);print('BUILT '+case.name,flush=True)
    for title,_,_,_ in rig.CASES:rig.run(out/f'{title}-{suffix}')
    rig.compare(out,suffix)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--stage',choices=['prepare','proof','baseline','candidate','clean'],required=True);p.add_argument('--suffix',default='candidate')
    p.add_argument('--predecoded',action='store_true')
    p.add_argument('--branches',action='store_true');p.add_argument('--latest',action='store_true')
    p.add_argument('--writes',action='store_true');p.add_argument('--baseline-from',type=Path)
    a=p.parse_args();out=a.output.resolve()
    if a.stage=='prepare':prepare(out,a.predecoded,a.branches,a.latest,a.writes,a.baseline_from)
    elif a.stage=='proof':proof(out)
    elif a.stage=='baseline':baseline(out)
    elif a.stage=='clean':clean(out,a.suffix)
    else:candidate(out,a.suffix)
