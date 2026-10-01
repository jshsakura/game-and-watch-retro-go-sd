#!/usr/bin/env python3
"""Relink bulk-on A and bulk-on + packed picker B from canonical object recipes."""
import json, os, shlex
from pathlib import Path
from build_pair import BIN, sha, checked, package
os.environ['PATH']='/opt/arm-gnu-toolchain/bin:'+os.environ['PATH']
root=Path('build/snes-device-audio-pair');root.mkdir(exist_ok=False)
recipes=json.loads(Path('build/snes-device-compile-link.json').read_text())
link=recipes['link'];objects=[Path(x) for x in link if x.endswith('.o')]
before={str(p):sha(p) for p in objects}
audio=next(shlex.split(line) for line in Path('build/snes-device-recipes.txt').read_text().splitlines() if 'arm-none-eabi-gcc -c ' in line and 'Core/Src/porting/snes/snes_audio_stretch.c' in line)
paths=[Path('build/snes/'+f) for f in ['spin_bake.o','spin_bake.d','snes_audio_stretch.o','snes_audio_stretch.d']]
backup={p:p.read_bytes() for p in paths}
commands=[]
try:
    checked(recipes['compile']+['-DSNES_BAKE_BULK=1'])
    checked([BIN+'objcopy','--redefine-syms=snes_redefines','build/snes/spin_bake.o'])
    A=package(Path('build/snes-device-pair/B/gw_retro_go.elf'),root/'A')
    compile=audio+['-DSNES_STRETCH_PACKED_PICK=1'];checked(compile)
    checked([BIN+'objcopy','--redefine-syms=snes_redefines','build/snes/snes_audio_stretch.o'])
    changed={'build/snes/spin_bake.o','build/snes/snes_audio_stretch.o'}
    assert all(sha(p)==before[str(p)] for p in objects if str(p) not in changed)
    elf=root/'candidate.elf'
    cmd=[str(elf) if x=='build/gw_retro_go.elf' else '-Wl,-Map='+str(root/'candidate.map')+',--cref' if x.startswith('-Wl,-Map=') else x for x in link]
    checked(cmd);os.environ['NM']=BIN+'nm'
    for script,args in [('check_core_symbol_aliases.py',['build',str(elf)]),('check_logo_index_alignment.py',[BIN+'nm',str(elf),'Core/Inc/retro-go/bitmaps.h']),('check_resident_init_array.py',[BIN+'objdump',str(elf)]),('check_no_resident_logo_refs.py',[BIN+'objdump',BIN+'nm',str(elf)]),('check_xip_sentinels.py',[BIN+'objdump',str(elf)])]: checked(['python3','scripts/'+script,*args])
    checked(['bash','scripts/check_snes_profile_wired.sh','build','0'])
    B=package(elf,root/'B')
    for name,hashes in [('A',A),('B',B)]:
        (root/name/'manifest.json').write_text(json.dumps(dict(warmup=120,frames=900,profiler=False,device_hook=1,bulk=1,packed_picker=int(name=='B'),silent_completion_required=True,sha256=hashes,method='bulk-on baseline; candidate changes only audio picker object; coherent cursor snapshot and SMLALD pairs',timing='identical firmware completion hook'),indent=2)+'\n')
    (root/'commands.json').write_text(json.dumps(dict(audio_compile=compile,bulk_compile=recipes['compile']+['-DSNES_BAKE_BULK=1'],candidate_link=cmd),indent=2)+'\n')
    (root/'objects.json').write_text(json.dumps(before,indent=2)+'\n')
finally:
    for p,data in backup.items():p.write_bytes(data)
assert all(sha(p)==before[str(p)] for p in objects)
print('Audio A/B packaged; cached baseline objects restored')
