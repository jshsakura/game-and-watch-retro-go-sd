#!/usr/bin/env python3
"""Build a finite-hook-free SNES experiment pair; never flash it."""
import os,json,shlex
from pathlib import Path
from build_pair import BIN,sha,checked,package
os.environ['PATH']='/opt/arm-gnu-toolchain/bin:'+os.environ['PATH']
root=Path('build/snes-playable-bulk-20261002');root.mkdir(exist_ok=False)
r=json.loads(Path('build/snes-device-compile-link.json').read_text());link=r['link']
objects=[Path(x) for x in link if x.endswith('.o')];before={str(p):sha(p) for p in objects}
compile=next(shlex.split(line) for line in Path('build/snes-device-recipes.txt').read_text().splitlines() if 'arm-none-eabi-gcc -c ' in line and 'Core/Src/porting/snes/main_snes.c' in line)
assert '-DSNES_DEVICE_BENCH=1' in compile
compile=['-DSNES_DEVICE_BENCH=0' if x=='-DSNES_DEVICE_BENCH=1' else x for x in compile]
paths=[Path('build/snes/'+f) for f in ['main_snes.o','main_snes.d','spin_bake.o','spin_bake.d']]
backup={p:p.read_bytes() for p in paths}
try:
    checked(r['compile']+['-DSNES_BAKE_BULK=1']);checked([BIN+'objcopy','--redefine-syms=snes_redefines','build/snes/spin_bake.o'])
    checked(compile);checked([BIN+'objcopy','--redefine-syms=snes_redefines','build/snes/main_snes.o'])
    assert all(sha(p)==before[str(p)] for p in objects if p.name not in ['spin_bake.o','main_snes.o'])
    elf=root/'candidate.elf'
    cmd=[str(elf) if x=='build/gw_retro_go.elf' else '-Wl,-Map='+str(root/'candidate.map')+',--cref' if x.startswith('-Wl,-Map=') else x for x in link]
    checked(cmd);os.environ['NM']=BIN+'nm'
    for script,args in [('check_core_symbol_aliases.py',['build',str(elf)]),('check_logo_index_alignment.py',[BIN+'nm',str(elf),'Core/Inc/retro-go/bitmaps.h']),('check_resident_init_array.py',[BIN+'objdump',str(elf)]),('check_no_resident_logo_refs.py',[BIN+'objdump',BIN+'nm',str(elf)]),('check_xip_sentinels.py',[BIN+'objdump',str(elf)])]: checked(['python3','scripts/'+script,*args])
    checked(['bash','scripts/check_snes_profile_wired.sh','build','0'])
    import subprocess
    symbols=subprocess.check_output([BIN+'nm',str(elf)],text=True)
    assert all(x not in symbols for x in ['snes_device_bench_result','common_emu_bench_begin','common_emu_bench_complete'])
    hashes=package(elf,root/'arm')
    (root/'arm/manifest.json').write_text(json.dumps(dict(sha256=hashes,bulk=1,packed_picker=0,device_hook=0,profiler=False,clock='existing default 312 MHz; choose supported CPU menu level 2 for 340 MHz',device_play_validation='not performed for this hook-free relink; the matching instrumented bulk candidate reached 60.13 drawn FPS at 340 MHz',release=False,warning='isolated old-base SNES experiment firmware/core pair, not a main/32X release; keep the pair together'),indent=2)+'\n')
    (root/'commands.json').write_text(json.dumps(dict(main_compile=compile,link=cmd),indent=2)+'\n')
finally:
    for p,data in backup.items():p.write_bytes(data)
assert all(sha(p)==before[str(p)] for p in objects)
print('Playable experiment pair built; no benchmark-stop symbols; baseline intermediates restored')
