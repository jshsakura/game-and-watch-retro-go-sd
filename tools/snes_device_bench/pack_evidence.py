#!/usr/bin/env python3
"""Archive completed, ROM-free device evidence and source provenance."""
import gzip,hashlib,json,shutil,subprocess
from pathlib import Path
root=Path('tools/snes_device_bench/results/20261002');root.mkdir(parents=True,exist_ok=False)
cases=['snes-device-zelda-20261001','snes-device-smw-20261002','snes-device-smw-r2-20261002','snes-device-smw-r3-20261002','snes-device-dragons-20261002','snes-device-smw-alldraw-20261002','snes-device-smw-packed-current-20261002','snes-device-smw-340-alldraw-20261002','snes-device-smw-340-default-20261002','snes-device-zelda-340-alldraw-20261002']
def copy(src,dest):
    dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dest)
for case in cases:
    src=Path('build')/case;dest=root/case;dest.mkdir()
    summary=json.loads((src/'result.json').read_text());assert summary['status']=='completed' and summary['original_restored']
    assert all(json.loads((src/'restore/readback.json').read_text()).values())
    for name in ['result.json','comparison.json','conditions.json','smoke-reuse.json']:
        if (src/name).exists():copy(src/name,dest/name)
    copy(src/'restore/readback.json',dest/'restore/readback.json')
    for arm in ['smoke','base-before','candidate','base-after']:
        p=src/arm
        if not p.exists():continue
        for name in ['result.json','flash.log','start.log']:copy(p/name,dest/arm/name)
        for name in ['report.json','record.bin','audio.bin','audio-hardware.json','collect.gdb','gdb.log','openocd.log','host-timer.json']:
            if (p/'capture'/name).exists():copy(p/'capture'/name,dest/arm/'capture'/name)
        screen=p/'capture/screen.bin'
        (dest/arm/'capture/screen.bin.gz').write_bytes(gzip.compress(screen.read_bytes(),mtime=0))
failed=Path('build/snes-device-smw-packed-20261002')
for name in ['conditions.json','failure.json']:copy(failed/name,root/'admission-refused'/name)
for pair in ['snes-device-pair','snes-device-audio-pair']:
    for arm in ['A','B']:copy(Path('build')/pair/arm/'manifest.json',root/pair/arm/'manifest.json')
    for name in ['commands.json','objects.json','sizes.json']:
        p=Path('build')/pair/name
        if p.exists():(root/(pair+'-'+name+'.gz')).write_bytes(gzip.compress(p.read_bytes(),mtime=0))
copy(Path('build/snes-audio-validation/result.json'),root/'audio-stream-validation.json')
for name in ['snes-audio-qemu.log','snes-audio-build.log','snes-playable-build.log','snes-final-cases-plan-20261002.json','snes-device-smw-aggregate-20261002.json']:
    copy(Path('build')/name,root/name)
copy(Path('build/snes-playable-bulk-20261002/arm/manifest.json'),root/'playable-manifest.json')
shutil.copytree(Path('build/snes-device-pair/provenance'),root/'initial-provenance')
prov=root/'current-provenance';prov.mkdir()
(prov/'root.diff').write_bytes(subprocess.check_output(['git','diff','--binary']))
copy(Path('Core/Src/porting/snes/snes_device_bench.h'),prov/'snes_device_bench.h')
for p in Path('tools/snes_device_bench').glob('*.py'):copy(p,prov/p.name)
copy(Path('tools/snes_device_bench/test_audio_picker.c'),prov/'test_audio_picker.c')
copy(Path('tools/snes_device_bench/audio_shim/main.h'),prov/'audio_shim/main.h')
files={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in root.rglob('*') if p.is_file()}
manifest=dict(root_commit=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),root_branch=subprocess.check_output(['git','branch','--show-current'],text=True).strip(),sm_commit=subprocess.check_output(['git','-C','external/sm','rev-parse','HEAD'],text=True).strip(),cases=cases,files=files,private_inputs='ROMs/full saves/CONFIG/firmware dumps excluded; originals stay in local build directories',observations=['User: sound seemed flat during full-render diagnostic; later said it sounded better. The heard arm and exact interval were not logged, so this is session feedback, not a blinded A/B sound result.'],limitations=['endpoint hashes are not full hardware audio streams','forced_draw_diagnostic records must not be described as default adaptive-mode performance','post-completion stretcher step/base/fill are after state CRC work and cannot characterize steady-state playback rate'])
(root/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')
print('Archived',len(cases),'completed cases plus preserved admission failure;',len(files),'ROM-free evidence files')
