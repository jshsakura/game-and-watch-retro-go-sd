"""Build the tree that will be committed (wt-main) as a bench arm."""
from pathlib import Path
import subprocess,shutil,json,re,struct,hashlib,sys
o=Path('/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/build/32x-mixer-hle-20261001');w=o/'wt-main'
name=sys.argv[1];out=o/'arms-bench'/name;out.mkdir(parents=True,exist_ok=False)
m=json.loads((o/'arms-bench/hle3a/manifest.json').read_text())
base=['docker','run','--rm','--init','--user','1000:1000','-e','GIT_TAG_OVERRIDE=32x-'+name+'-20261001','-v',str(w)+':/opt/workdir','sylverb/retro-go-sd-builder:v1.5']
with (out/'build.log').open('w') as log:
  subprocess.run(base+['make','-j3','all']+m['flags'],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=3600)
  for suffix,sects in [('core.raw',['.overlay_md32x','.overlay_md32x_itc']),('32x.xip',['.xip_md32x','.rodata_md32x'])]:
    subprocess.run(base+['/opt/arm-gnu-toolchain/bin/arm-none-eabi-objcopy','-O','binary']+['--only-section='+s for s in sects]+['build/gw_retro_go.elf','build/'+suffix],stdout=log,stderr=subprocess.STDOUT,check=True)
for n in ['gw_retro_go.elf','gw_retro_go_intflash.bin','32x.xip']:shutil.copy2(w/'build'/n,out/n)
tag=re.search(r'#define GIT_TAG "(.*)"',(w/'Core/Inc/gittag.h').read_text()).group(1).encode()
(out/'32x.bin').write_bytes(b'CORI'+struct.pack('<HHB',0,len(tag)+1,len(tag))+tag+(w/'build/core.raw').read_bytes())
m['sha256']={n:hashlib.sha256((out/n).read_bytes()).hexdigest() for n in ['gw_retro_go.elf','gw_retro_go_intflash.bin','32x.bin','32x.xip']}
m['source']='wt-main working tree (perf/32x-hle-itcm + uncommitted ld/alloc changes), picodrive 81c91dc3'
(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n');print('built',out,flush=True)
