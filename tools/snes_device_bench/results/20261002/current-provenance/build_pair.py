#!/usr/bin/env python3
"""Package a canonical baseline and relink a one-object bulk-fold variant.

Run inside the pinned builder after `make all` and `make -nB` recipe capture.
Other linked objects are kept identical. Restore the baseline intermediates even
if building the candidate fails. No device access.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

BIN = '/opt/arm-gnu-toolchain/bin/arm-none-eabi-'

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def checked(argv):
    subprocess.run(argv, check=True)

def package(elf, dest):
    dest.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(elf, dest/'gw_retro_go.elf')
    checked([BIN+'objcopy','-O','binary','-S', *sum((['-j',s] for s in
        ['.isr_vector','.firmware_abi','.text','.rodata','.ARM.extab',
         '.preinit_array','.init_array','.fini_array','.data']),[]),
        str(elf), str(dest/'gw_retro_go_intflash.bin')])
    raw=dest/'snes.raw'
    checked([BIN+'objcopy','-O','binary','--only-section=.overlay_snes',
        '--only-section=.itcm_rc_hot','--only-section=.itcm_snes_interp',str(elf),str(raw)])
    tag=re.search(r'^#define GIT_TAG "(.*)"$',Path('Core/Inc/gittag.h').read_text(),re.M)[1].encode()
    assert len(tag)<=255
    (dest/'snes.bin').write_bytes(struct.pack('<4sHHB',b'CORI',0,len(tag)+1,len(tag))+tag+raw.read_bytes())
    raw.unlink()
    assert 0<(dest/'gw_retro_go_intflash.bin').stat().st_size<=262144
    return {p.name:sha(p) for p in dest.iterdir() if p.is_file()}

def main():
    os.environ['PATH']='/opt/arm-gnu-toolchain/bin:'+os.environ['PATH']
    root=Path('build/snes-device-pair')
    root.mkdir(exist_ok=False)
    recipes=json.loads(Path('build/snes-device-compile-link.json').read_text())
    link=recipes['link']
    objects=[Path(x) for x in link if x.endswith('.o')]
    before={str(p):sha(p) for p in objects}
    baseline=package(Path('build/gw_retro_go.elf'),root/'A')
    assert baseline['gw_retro_go_intflash.bin']==sha(Path('build/gw_retro_go_intflash.bin'))
    backups={p:p.read_bytes() for p in [Path('build/snes/spin_bake.o'),Path('build/snes/spin_bake.d')]}
    try:
        compile=recipes['compile']+['-DSNES_BAKE_BULK=1']
        assert not any('BAKE_BULK=' in a for a in recipes['compile'])
        checked(compile)
        checked([BIN+'objcopy','--redefine-syms=snes_redefines','build/snes/spin_bake.o'])
        candidate_object=sha(Path('build/snes/spin_bake.o'))
        assert candidate_object!=before['build/snes/spin_bake.o']
        for p in objects:
            if str(p)!='build/snes/spin_bake.o':
                assert sha(p)==before[str(p)],str(p)+' changed'
        candidate_elf=root/'candidate.elf'
        candidate_link=[str(candidate_elf) if x=='build/gw_retro_go.elf' else
            '-Wl,-Map='+str(root/'candidate.map')+',--cref' if x.startswith('-Wl,-Map=') else x for x in link]
        checked(candidate_link)
        os.environ['NM']=BIN+'nm'
        for script,args in [
            ('check_core_symbol_aliases.py',['build',str(candidate_elf)]),
            ('check_logo_index_alignment.py',[BIN+'nm',str(candidate_elf),'Core/Inc/retro-go/bitmaps.h']),
            ('check_resident_init_array.py',[BIN+'objdump',str(candidate_elf)]),
            ('check_no_resident_logo_refs.py',[BIN+'objdump',BIN+'nm',str(candidate_elf)]),
            ('check_xip_sentinels.py',[BIN+'objdump',str(candidate_elf)])]:
            checked(['python3','scripts/'+script,*args])
        checked(['bash','scripts/check_snes_profile_wired.sh','build','0'])
        candidate=package(candidate_elf,root/'B')
        for name,hashes in [('A',baseline),('B',candidate)]:
            manifest=dict(warmup=120,frames=900,profiler=False,device_hook=1,
                silent_completion_required=True,bulk=int(name=='B'),sha256=hashes,
                method='canonical make-all A; B recompiles only spin_bake.c with SNES_BAKE_BULK=1; all other linked objects byte-identical',
                baseline_object_sha256=before['build/snes/spin_bake.o'],candidate_object_sha256=candidate_object,
                timing='HAL_GetTick at end of full paced iterations; final guest frame forced drawn in both arms')
            (root/name/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        (root/'commands.json').write_text(json.dumps(dict(baseline=recipes,candidate_compile=compile,candidate_link=candidate_link),indent=2)+'\n')
        (root/'objects.json').write_text(json.dumps(before,indent=2)+'\n')
    finally:
        for p,data in backups.items(): p.write_bytes(data)
    assert all(sha(p)==before[str(p)] for p in objects)
    print('A/B packaged; all baseline linked objects restored')

if __name__=='__main__': main()
