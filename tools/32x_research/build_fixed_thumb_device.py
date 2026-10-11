#!/usr/bin/env python3
"""Build immutable fixed-Thumb Doom A/B arms; no device access."""
import argparse,hashlib,json,re,shutil,struct,subprocess,traceback
from pathlib import Path
import run_aot_blocks as rig

ROOT=rig.ROOT
IMAGE='sylverb/retro-go-sd-builder@sha256:948725b822603810687c2b0331d0105c30167f4cf8157b210fd452f20db94691'
HELPERS=('sh2_fixed_step','sh2_fixed_decoded','sh2_fixed_irqtail')
BENCH=['-DGNW_32X_DEVICE_BENCH_WARMUP=900','-DGNW_32X_DEVICE_BENCH_FRAMES=600',
       '-DGNW_32X_DEVICE_BENCH_START_FRAME=600','-DGNW_32X_DEVICE_BENCH_START_FRAMES=8','-DGNW_32X_DEVICE_BENCH_START_PAD=16',
       '-DGNW_32X_DEVICE_BENCH_CONFIRM_FRAME=720','-DGNW_32X_DEVICE_BENCH_CONFIRM_FRAMES=8','-DGNW_32X_DEVICE_BENCH_CONFIRM_PAD=16',
       '-DGNW_32X_DEVICE_BENCH_CONFIRM2_FRAME=840']
FLAGS=['CHECK_DIRTY_SUBMODULE=0','COVERFLOW=1','SHARED_HIBERNATE_SAVESTATE=1','DISABLE_SPLASH_SCREEN=1',
       'ENABLE_BOOT_OC=1','INTFLASH_BANK=2','CHEAT_CODES=1','ZH_CN=1','ZH_TW=1','KO_KR=1','JA_JP=1',
       'MD32X_DEVICE_PROFILE=0','GNW_AUTOBOOT=0','GNW_NO_IDLE_OFF=1','GNW_NO_BOOT_RESCUE=1',
       'MD32X_EXTRA_DEFS='+' '.join(BENCH)]

def build(out, resume=False):
    costs=json.loads((out/'comparison-candidate-clean.json').read_text())
    assert all(r['cost_change_percent']<0 and r['screen_audio_equal_to_counted'] for r in costs)
    w=out/'firmware';assert w.exists() if resume else not w.exists()
    result=dict(status='preparing',device_access=False,image=IMAGE,flags=FLAGS,arms={},volume=0)
    docker=['docker','run','--rm','--init','--user','1000:1000','-e','GIT_TAG_OVERRIDE=32x-thumb-20261007',
            '-v',str(w)+':/opt/workdir',IMAGE]
    try:
        if not resume:subprocess.run(['cp','-a','--reflink=auto',str(ROOT/'build/32x-lookup-range-20261007/firmware'),str(w)],check=True,timeout=180)
        pd=w/'external/picodrive';cpu=pd/'cpu/sh2/mame/sh2pico.c'
        for name in ('sh2pico.c','sh2.c','pcm_span.h','pcm_synthetic.h'):
            shutil.copyfile(out/'baseline/cpu/sh2/mame'/name,pd/'cpu/sh2/mame'/name)
        # Existing private firmware has the validated finite, muted completion
        # hook. Every instruction/input/clock option is shared by both arms.
        main=w/'Core/Src/porting/md32x/main_md32x.c'
        assert 'audio_stop_playing' in (w/'Core/Src/porting/common.c').read_text()
        assert 'odroid_audio_volume_set(0)' in main.read_text()
        result['compiler']=subprocess.check_output(docker+['arm-none-eabi-gcc','--version'],text=True).splitlines()[0]
        for arm in ('baseline','candidate'):
            dst=out/'device-arms'/arm
            if resume and arm=='baseline':
                m=json.loads((dst/'manifest.json').read_text())
                assert all(rig.digest(dst/n)==v for n,v in m['sha256'].items())
                result['arms'][arm]=dict(path=str(dst),placement=m['placement']);continue
            dst.mkdir(parents=True,exist_ok=resume)
            with (dst/'build.log').open('w') as log:
                def run(args,timeout=1800):
                    subprocess.run(docker+args,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=timeout)
                if arm=='candidate':
                    cpu.write_text(''.join(f'#define {h} md32x__{h}\n' for h in HELPERS)+
                                   (out/'candidate/cpu/sh2/mame/sh2pico.c').read_text())
                    run(['make','build/md32x/cpu__sh2__mame__sh2pico.o']+FLAGS)
                    # GCC outlines these shared C handlers because two fallback
                    # entries call them. They cover operations already handled
                    # by Thumb; the original delay/IRQ paths still call them.
                    # Move six whole sections, with their relocations, to the
                    # existing cold-code region. Keep shifts/branches/hook
                    # dispatch and the memory bus in ITCM.
                    cold=['op0010','op0011','op0101','op1001','op1100','op1101']
                    run(['arm-none-eabi-objcopy',*[v for h in cold for v in
                        ['--rename-section',f'.text.{h}=.gnw_xip_cold']],
                        'build/md32x/cpu__sh2__mame__sh2pico.o'],180)
                    result['fallback_sections_in_xip']=cold
                    shutil.copyfile(out/'fixed_thumb.S',w/'build/fixed_thumb.S')
                    shutil.copyfile(out/'sh2_offsets.inc',w/'build/sh2_offsets.inc')
                    run(['arm-none-eabi-gcc',*rig.ARCH,'-Ibuild',*[f'-D{h}=md32x__{h}' for h in HELPERS],
                         '-c','build/fixed_thumb.S','-o','build/fixed_thumb.o'],180)
                    run(['arm-none-eabi-objcopy','--redefine-syms=md32x_redefines','build/fixed_thumb.o'],180)
                    run(['arm-none-eabi-gcc',*rig.ARCH,'-r','-nostdlib','build/md32x/cpu__sh2__mame__sh2pico.o',
                         'build/fixed_thumb.o','-o','build/fixed_thumb_merged.o'],180)
                    shutil.move(w/'build/fixed_thumb_merged.o',w/'build/md32x/cpu__sh2__mame__sh2pico.o')
                run(['make','-j3','all']+FLAGS)
                for name,sections in [('core.raw',['.overlay_md32x','.overlay_md32x_itc']),
                                      ('32x.xip',['.xip_md32x','.rodata_md32x'])]:
                    run(['arm-none-eabi-objcopy','-O','binary',*['--only-section='+s for s in sections],
                         'build/gw_retro_go.elf','build/'+name],180)
            for name in ('gw_retro_go.elf','gw_retro_go_intflash.bin','32x.xip','gw_retro_go.map'):
                shutil.copyfile(w/'build'/name,dst/name)
            symbols=subprocess.check_output(docker+['arm-none-eabi-nm','-S','build/gw_retro_go.elf'],text=True)
            (dst/'symbols.txt').write_text(symbols)
            values={}
            for line in symbols.splitlines():
                words=line.split()
                if len(words)>=3:
                    try:values[words[-1]]=int(words[0],16)
                    except ValueError:pass
            assert values['__md32x_itc_bss_end__']<=0x10000
            assert values['_OVERLAY_MD32X_BSS_END']<values['__RAM_EMU_END__']
            if arm=='candidate':
                assert 0<values['md32x__sh2_execute_interpreter']<0x10000
                assert all(values[h if h.startswith('md32x__') else 'md32x__'+h]>0x10000 for h in HELPERS)
            tag=re.search(r'#define GIT_TAG "(.*)"',(w/'Core/Inc/gittag.h').read_text())[1].encode()
            (dst/'32x.bin').write_bytes(b'CORI'+struct.pack('<HHB',0,len(tag)+1,len(tag))+tag+(w/'build/core.raw').read_bytes())
            placement={k:hex(values[k]) for k in ('__md32x_itc_bss_end__','_OVERLAY_MD32X_BSS_END','__RAM_EMU_END__')}
            meta=dict(variant=arm,warmup=900,frames=600,profiler=False,device_hook=1,silent_completion_required=True,
                      flags=FLAGS,tag=tag.decode(),compiler=result['compiler'],image=IMAGE,volume=0,load_state=False,
                      startup_input='B f600..607,720..727,840..847',picodrive_commit=json.loads((out/'sources.json').read_text())['core_commit'],
                      source_sha256=rig.digest(cpu),main_sha256=rig.digest(main),placement=placement,
                      sha256={n:rig.digest(dst/n) for n in ('gw_retro_go.elf','gw_retro_go_intflash.bin','32x.bin','32x.xip')})
            rig.save(dst/'manifest.json',meta);result['arms'][arm]=dict(path=str(dst),placement=placement)
            print('BUILT device '+arm,flush=True)
        result['status']='built'
    except Exception as e:
        result.update(status='failed',error=str(e));(out/'device-build-error.log').write_text(traceback.format_exc())
    rig.save(out/'device-build-result.json',result);print(json.dumps(result),flush=True)
    if result['status']!='built':raise SystemExit(2)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--resume-candidate',action='store_true');a=p.parse_args()
    build(a.output.resolve(),a.resume_candidate)
