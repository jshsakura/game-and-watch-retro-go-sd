#!/usr/bin/env python3
"""Full post-stretcher PCM differential checks with real game PCM and bursts."""
import ctypes as C, hashlib, json, subprocess
from pathlib import Path
out=Path('build/snes-audio-validation');out.mkdir(exist_ok=False)
source='Core/Src/porting/snes/snes_audio_stretch.c'
libs=[]
for packed in [0,1]:
    dest=out/('audio-%d.so'%packed)
    subprocess.run(['gcc','-O2','-g','-shared','-fPIC','-DTARGET_GNW','-DSNES_STRETCH_PACKED_PICK=%d'%packed,'-Itools/snes_device_bench/audio_shim',source,'-o',str(dest)],check=True)
    lib=C.CDLL(str(dest.resolve()))
    lib.snes_stretch_push.argtypes=[C.POINTER(C.c_int16),C.c_uint16]
    lib.snes_stretch_pull.argtypes=[C.POINTER(C.c_int16),C.c_uint16]
    libs.append(lib)
records=[]
for title in ['zelda-default','smw','kart','dragonmagic','metroid','dkc','fzero','pilotwings','ff5-short']:
    pcm=Path('build/snes-harness-ready')/(title+'-A')/'audio.pcm'
    data=pcm.read_bytes(); n=len(data)//532
    for schedule in ['60','58.586','54.32','44','90','bursts','toggle']:
        for mode in [0,1]:
            for lib in libs:
                lib.snes_stretch_reset();C.c_uint8.in_dll(lib,'g_snes_audio_gapfree').value=mode
            digests=[hashlib.sha256(),hashlib.sha256()];acc=0.0; pulls=0
            for frame in range(n):
                inp=(C.c_int16*266).from_buffer_copy(data[frame*532:(frame+1)*532])
                if schedule=='toggle' and frame%37==0:
                    for lib in libs:C.c_uint8.in_dll(lib,'g_snes_audio_gapfree').value=(mode+frame//37)%2
                for lib in libs:lib.snes_stretch_push(inp,266)
                period=0 if schedule=='bursts' and frame%16<8 else 2 if schedule=='bursts' else 60.15/float(schedule if schedule!='toggle' else 54.32)
                acc+=period
                while acc>=1:
                    acc-=1; samples=[]
                    for lib,h in zip(libs,digests):
                        buf=(C.c_int16*266)();lib.snes_stretch_pull(buf,266);samples.append(bytes(buf));h.update(bytes(buf))
                    assert samples[0]==samples[1],(title,schedule,mode,frame,'PCM')
                    pulls+=1
                diagnostics=[[lib.snes_stretch_fill(),lib.snes_stretch_step_q16(),lib.snes_stretch_underruns(),*[C.c_uint32.in_dll(lib,x).value for x in ['g_stretch_ins','g_stretch_rev','g_stretch_conf_lp']]] for lib in libs]
                assert diagnostics[0]==diagnostics[1],(title,schedule,mode,frame,diagnostics)
            assert digests[0].digest()==digests[1].digest()
            records.append(dict(title=title,source_sha256=hashlib.sha256(data).hexdigest(),schedule=schedule,initial_gapfree=mode,frames=n,pulls=pulls,pcm_sha256=digests[0].hexdigest(),identical_pcm_and_diagnostics=True))
(out/'result.json').write_text(json.dumps(dict(status='passed',cases=len(records),records=records,limitation='deterministic schedules exclude concurrent ISR cursor changes; native SMLALD adapter uses exact signed halfword arithmetic'),indent=2)+'\n')
print('PASS',len(records),'full PCM schedules including overflow bursts and runtime mode changes')
