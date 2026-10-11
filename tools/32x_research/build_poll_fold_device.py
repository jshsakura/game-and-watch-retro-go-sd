#!/usr/bin/env python3
"""Build only S01's device candidate; reuse the immutable validated A arm.

No device access. Exact prior GCC image, flags, finite muted completion hooks,
and unchanged production sources. No handler moves or Thumb interpreter.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import traceback

import run_aot_blocks as rig
from build_fixed_thumb_device import IMAGE, FLAGS

ROOT = rig.ROOT
PRIOR = ROOT / 'build/32x-fixed-thumb-v4-20261007'
TESTED = ROOT / 'build/32x-poll-fold-20261008/run-000002'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tested', type=Path, default=TESTED)
    a = parser.parse_args()
    out = a.output.resolve()
    tested = a.tested.resolve()
    tested.relative_to(ROOT)
    out.relative_to(ROOT)
    out.mkdir(parents=True, exist_ok=False)
    state = dict(status='preparing_device_candidate', hypothesis='S01', device_access=False,
                 physical_fps_measured=False, automatic_retry=False,
                 rig_result=str(tested / 'state.json'))
    rig.save(out / 'state.json', state)
    result = dict(status='preparing', device_access=False, image=IMAGE, flags=FLAGS, arms={})
    try:
        tested_state = json.loads((tested / 'state.json').read_text())
        assert tested_state['proof']['cases'] == 65536
        assert rig.digest(tested / 'candidate/cpu/sh2/mame/sh2pico.c') == tested_state['candidate_sha256']
        for title in ['doom', 'after-burner']:
            comparison = json.loads((tested / f'{title}-comparison.json').read_text())
            assert comparison['status'] == 'completed_clean_comparison'
        base = PRIOR / 'device-arms/baseline'
        meta = json.loads((base / 'manifest.json').read_text())
        assert all(rig.digest(base / n) == h for n, h in meta['sha256'].items())
        assert meta['flags'] == FLAGS and meta['volume'] == 0
        dst = out / 'device-arms/baseline'
        shutil.copytree(base, dst)
        result['arms']['baseline'] = dict(path=str(dst), reused_from=str(base), placement=meta['placement'])
        shutil.copytree(PRIOR / 'device-tools', out / 'device-tools')
        # Existing run_fixed_thumb_device's backup/restore/finite A/B/A runner
        # is reusable; its only rig dependency here is verified Doom identity.
        (out / 'doom-baseline').mkdir()
        shutil.copyfile(PRIOR / 'doom-baseline-clean/inputs.json', out / 'doom-baseline/inputs.json')
        w = out / 'firmware'
        subprocess.run(['cp', '-a', '--reflink=auto', str(PRIOR / 'firmware'), str(w)],
                       check=True, timeout=180)
        cpu = w / 'external/picodrive/cpu/sh2/mame/sh2pico.c'
        for name in ['sh2.c', 'pcm_span.h', 'pcm_synthetic.h']:
            original = PRIOR / 'baseline/cpu/sh2/mame' / name
            target = cpu.with_name(name)
            if rig.digest(original) != rig.digest(target):
                shutil.copyfile(original, target)
        shutil.copyfile(tested / 'candidate/cpu/sh2/mame/sh2pico.c', cpu)
        main = w / 'Core/Src/porting/md32x/main_md32x.c'
        assert rig.digest(main) == meta['main_sha256']
        assert 'audio_stop_playing' in (w / 'Core/Src/porting/common.c').read_text()
        docker = ['docker', 'run', '--rm', '--init', '--user', '1000:1000',
                  '-e', 'GIT_TAG_OVERRIDE=32x-thumb-20261007',
                  '-v', str(w) + ':/opt/workdir', IMAGE]
        dst = out / 'device-arms/candidate'
        dst.mkdir()
        with (dst / 'build.log').open('w') as log:
            # docker run fetches the exact pinned image once if absent.
            version = subprocess.run(docker + ['arm-none-eabi-gcc', '--version'],
                                     stdout=subprocess.PIPE, stderr=log, text=True,
                                     check=True, timeout=600).stdout.splitlines()[0]
            assert version == meta['compiler'], (version, meta['compiler'])
            result['compiler'] = version
            def run(args, timeout=1800):
                subprocess.run(docker + args, stdout=log, stderr=subprocess.STDOUT,
                               check=True, timeout=timeout)
            run(['make', 'build/md32x/cpu__sh2__mame__sh2pico.o', *FLAGS])
            run(['make', '-j3', 'all', *FLAGS])
            for name, sections in [('core.raw', ['.overlay_md32x', '.overlay_md32x_itc']),
                                   ('32x.xip', ['.xip_md32x', '.rodata_md32x'])]:
                run(['arm-none-eabi-objcopy', '-O', 'binary',
                     *['--only-section=' + s for s in sections],
                     'build/gw_retro_go.elf', 'build/' + name], 180)
        for name in ['gw_retro_go.elf', 'gw_retro_go_intflash.bin', '32x.xip', 'gw_retro_go.map']:
            shutil.copyfile(w / 'build' / name, dst / name)
        symbols = subprocess.check_output(['arm-none-eabi-nm', '-S', str(dst / 'gw_retro_go.elf')], text=True)
        (dst / 'symbols.txt').write_text(symbols)
        values = {}
        for line in symbols.splitlines():
            words = line.split()
            if len(words) >= 3:
                try:
                    values[words[-1]] = int(words[0], 16)
                except ValueError:
                    pass
        assert values['__md32x_itc_bss_end__'] <= 0x10000
        assert values['_OVERLAY_MD32X_BSS_END'] < values['__RAM_EMU_END__']
        assert 0 < values['md32x__sh2_execute_interpreter'] < 0x10000
        assert not any('sh2_fixed' in k for k in values), 'Unrelated rejected Thumb implementation leaked into candidate'
        tag = re.search(r'#define GIT_TAG "(.*)"', (w / 'Core/Inc/gittag.h').read_text())[1].encode()
        assert tag.decode() == meta['tag']
        (dst / '32x.bin').write_bytes(b'CORI' + struct.pack('<HHB', 0, len(tag) + 1, len(tag)) +
                                    tag + (w / 'build/core.raw').read_bytes())
        placement = {k: hex(values[k]) for k in ['__md32x_itc_bss_end__', '_OVERLAY_MD32X_BSS_END', '__RAM_EMU_END__']}
        candidate_meta = dict(meta, variant='S01-compact', source_sha256=rig.digest(cpu),
                              placement=placement,
                              sha256={n: rig.digest(dst / n) for n in meta['sha256']})
        rig.save(dst / 'manifest.json', candidate_meta)
        result['arms']['candidate'] = dict(path=str(dst), placement=placement)
        result['status'] = 'built'
        state.update(status='device_arms_built', last_result=str(out / 'device-build-result.json'))
    except Exception as error:
        result.update(status='failed', error=str(error), automatic_retry=False)
        state.update(status='device_build_failed', error=str(error))
        (out / 'device-build-error.log').write_text(traceback.format_exc())
    rig.save(out / 'device-build-result.json', result)
    rig.save(out / 'state.json', state)
    print(json.dumps(result), flush=True)
    if result['status'] != 'built':
        raise SystemExit(2)


if __name__ == '__main__':
    main()
