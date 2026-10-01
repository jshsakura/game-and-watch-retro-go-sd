"""Build Doom bench arms (warmup 900 / window 600, START pad at 600/720/840) from copies of two trees."""
from pathlib import Path
import subprocess,shutil,json,re,struct,hashlib,sys
o=Path('/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/build/32x-mixer-hle-20261001')
DEFS=('MD32X_EXTRA_DEFS=-DGNW_32X_DEVICE_BENCH_WARMUP=900 -DGNW_32X_DEVICE_BENCH_FRAMES=600 -DGNW_32X_DEVICE_BENCH_START_FRAME=600 '
      '-DGNW_32X_DEVICE_BENCH_START_FRAMES=8 -DGNW_32X_DEVICE_BENCH_START_PAD=16 -DGNW_32X_DEVICE_BENCH_CONFIRM_FRAME=720 '
      '-DGNW_32X_DEVICE_BENCH_CONFIRM_FRAMES=8 -DGNW_32X_DEVICE_BENCH_CONFIRM2_FRAME=840')
ref=json.loads((o/'arms-bench/hle3a/manifest.json').read_text())
flags=[f for f in ref['flags'] if not f.startswith('MD32X_EXTRA_DEFS=')]+[DEFS]
for src,name in zip(sys.argv[1::2],sys.argv[2::2]):
    w=o/('doomsrc-'+name)
    if not w.exists(): shutil.copytree(o/src,w,symlinks=True)
    for p in (w/'build/md32x').glob('*.o'):p.unlink()
    for n in ['rg_emulators','rg_main','main','odroid_system','common']:(w/'build/core'/(n+'.o')).unlink(missing_ok=True)
    out=o/'arms-doom'/name;out.mkdir(parents=True,exist_ok=False)
    base=['docker','run','--rm','--init','--user','1000:1000','-e','GIT_TAG_OVERRIDE=32x-doom-'+name,'-v',str(w)+':/opt/workdir','sylverb/retro-go-sd-builder:v1.5']
    with (out/'build.log').open('w') as log:
        subprocess.run(base+['make','-j3','all']+flags,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=3600)
        for suffix,sects in [('core.raw',['.overlay_md32x','.overlay_md32x_itc']),('32x.xip',['.xip_md32x','.rodata_md32x'])]:
            subprocess.run(base+['/opt/arm-gnu-toolchain/bin/arm-none-eabi-objcopy','-O','binary']+['--only-section='+s for s in sects]+['build/gw_retro_go.elf','build/'+suffix],stdout=log,stderr=subprocess.STDOUT,check=True)
    for n in ['gw_retro_go.elf','gw_retro_go_intflash.bin','32x.xip']:shutil.copy2(w/'build'/n,out/n)
    tag=re.search(r'#define GIT_TAG "(.*)"',(w/'Core/Inc/gittag.h').read_text()).group(1).encode()
    (out/'32x.bin').write_bytes(b'CORI'+struct.pack('<HHB',0,len(tag)+1,len(tag))+tag+(w/'build/core.raw').read_bytes())
    m=dict(ref);m.update(warmup=900,frames=600,flags=flags,source=f'{name}: copy of {src} built with the Doom bench defines',
        startup_input=[{'completed_frames':[600,608],'pad':16},{'completed_frames':[720,728],'pad':16},{'completed_frames':[840,848],'pad':16}])
    m['sha256']={n:hashlib.sha256((out/n).read_bytes()).hexdigest() for n in ['gw_retro_go.elf','gw_retro_go_intflash.bin','32x.bin','32x.xip']}
    (out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n');print('built',name,flush=True)
