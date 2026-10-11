#!/usr/bin/env python3
"""Prepare a clean, ROM-selected core snapshot without rig observers or proofs."""
import argparse,hashlib,json,shutil
from pathlib import Path
import native_math
from native_selection import select
ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--compact-code',action='store_true')
    args=parser.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    selection=select(args.rom.read_bytes());enabled=selection['enabled']
    core=out/'picodrive'
    shutil.copytree(ROOT/'external/picodrive',core,ignore=shutil.ignore_patterns('.git','*.o','*.a'))
    p=core/'cpu/sh2/mame/sh2pico.c';text=p.read_text();routes=[];headers=[]
    if enabled['countdown_fold']:
        headers.append('countdown_fold.h')
        text=text.replace('\t\tRIG_OPCOST_T1(sh2, opcode);','\t\tif(gnw_direct && opcode==0x8bfa && sh2->pc+8==sh2->ppc)countdown_fold(sh2);\n\t\tRIG_OPCOST_T1(sh2, opcode);',1)
    if enabled['pixel_native']:
        headers.append('pixel_native.h');routes.append((6,0x612c,'pixel_native(sh2,sh2->ppc)'))
    if enabled['control_native']:
        headers.append('control_native.h');routes.extend([(2,0x201f,'control_native(sh2,sh2->ppc,0)'),(3,0x3d88,'control_native(sh2,sh2->ppc,1)'),(5,0x52e1,'control_native(sh2,sh2->ppc,2)')])
    if enabled['lookup_native']:
        headers.append('lookup_native.h');routes.append((0,0x010d,'lookup_native(sh2,sh2->ppc)'))
    if enabled['chain_native']:
        headers.append('chain_native.h');routes.append((5,0x50e6,'chain_native(sh2,sh2->ppc)'))
    if enabled['pcm_tail_native']:
        headers.append('pcm_tail_native.h');routes.append((13,0xd21c,'pcm_tail_native(sh2,sh2->ppc)'))
    if headers:
        prefix='#include <stdint.h>\n#include <string.h>\n#include <stdlib.h>\n#ifndef GNW_SH2_FASTLOOPS\n#error Native kernels require existing GNW fastloop direct-fetch tracking\n#endif\n'
        if enabled['pixel_tail']:prefix+='#define PIXEL_NATIVE_TAIL 1\n'
        if enabled['inactive_fold']:prefix+='#define NATIVE_INACTIVE_FOLD 1\n'
        pieces=[]
        for name in headers:
            part=(HERE/name).read_text()
            if args.compact_code:
                for stats in ('countdown_iterations+=k;countdown_calls++;','pixel_calls++;pixel_groups+=n;','control_calls[kind]++;control_taken[kind]+=branch;','lookup_calls++;lookup_groups+=n;','chain_calls++;chain_parts+=parts;','inactive_calls++;inactive_iterations+=n;','pcm_tail_calls++;'):
                    part=part.replace(stats,'')
            pieces.append(part)
        prefix+='\n'.join(pieces)
        # Proof functions are absent unless explicitly defined; all counters
        # have no reporter here and can be eliminated by the compiler.
        signature='int sh2_execute_interpreter(SH2 *sh2, int cycles)\n{'
        assert text.count(signature)==2
        text=text.replace(signature,prefix+'\n'+signature,1)
        for major in {r[0] for r in routes}:
            name='op'+format(major,'04b');gate=f'\t\tcase 0x{major:X}: {name}(sh2, opcode); break;'
            assert text.count(gate)==1,gate
            tests=' '.join(f'if(gnw_direct && opcode==0x{op:04x} && {call}) {{ RIG_OPCOST_T1(sh2,opcode);continue; }}' for group,op,call in routes if group==major)
            text=text.replace(gate,f'\t\tcase 0x{major:X}: {tests} {name}(sh2,opcode);break;',1)
        if enabled['native_math']:
            m=core/'cpu/sh2/mame/sh2.c';m.write_text(native_math.inject(m.read_text()))
        if args.compact_code:
            m=core/'cpu/sh2/mame/sh2.c';body=m.read_text()
            for name in ('op0010','op0110','op1100'):
                body=body.replace('INLINE void '+name+'(', 'INLINE __attribute__((always_inline)) void '+name+'(',1)
            m.write_text(body)
    p.write_text(text)
    selection['rom_sha256']=hashlib.sha256(args.rom.read_bytes()).hexdigest()
    selection['core_source_hashes']={str(p.relative_to(core)):hashlib.sha256(p.read_bytes()).hexdigest() for p in core.rglob('*.c')}
    selection['compact_code']=args.compact_code
    selection['device_access']=False
    selection['deployment_status']='Core snapshot only; device loader selection, full firmware bank links and hardware A/B remain pending.'
    (out/'selection.json').write_text(json.dumps(selection,indent=2)+'\n')
    print(json.dumps({'variant':selection['variant'],'core':str(core),'device_access':False}))

if __name__=='__main__':main()
