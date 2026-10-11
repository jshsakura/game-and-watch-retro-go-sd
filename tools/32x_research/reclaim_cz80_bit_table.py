#!/usr/bin/env python3
"""Remove the redundant BIT flag table in an isolated normal S01 firmware.

Reuse completed S01 evidence. Exhaustively compare flags using the original
initialization code, then link with the actual release compiler. No device I/O.
"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import re
import subprocess
import traceback

from build_fixed_thumb_device import IMAGE

ROOT = Path(__file__).resolve().parents[2]
NORMAL = ROOT.parent / 'game-and-watch-retro-go-sd-pcm-20261005'
S01 = ROOT / 'build/32x-poll-fold-20261008/run-000003/candidate/cpu/sh2/mame/sh2pico.c'
FLAGS = ['CHECK_DIRTY_SUBMODULE=0', 'COVERFLOW=1', 'SHARED_HIBERNATE_SAVESTATE=1',
         'DISABLE_SPLASH_SCREEN=1', 'ENABLE_BOOT_OC=1', 'INTFLASH_BANK=2',
         'CHEAT_CODES=1', 'ZH_CN=1', 'ZH_TW=1', 'KO_KR=1', 'JA_JP=1']


def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def save(p, value):
    p.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def inspect_completed_build(w, evidence):
    elf = w / 'build/gw_retro_go.elf'
    symbols = subprocess.check_output(['arm-none-eabi-nm', '-S', str(elf)], text=True)
    (evidence / 'symbols.txt').write_text(symbols)
    values, sizes = {}, {}
    for line in symbols.splitlines():
        words = line.split()
        if len(words) >= 3:
            values[words[-1]] = int(words[0], 16)
            if len(words) == 4:
                sizes[words[-1]] = int(words[1], 16)
    # Other overlays may have their own local symbol with the same name.
    # Check the actual 32X object rather than the entire multi-core ELF.
    cz80_symbols = subprocess.check_output(['arm-none-eabi-nm', '-S',
        str(w / 'build/md32x/cpu__cz80__cz80.o')], text=True)
    assert not any(line.split()[-1] == 'SZ_BIT' for line in cz80_symbols.splitlines())
    assert not any('bench_result' in name or 'sh2_fixed' in name or 'common_emu_bench_' in name for name in values)
    assert values['__md32x_itc_bss_end__'] <= 0x10000
    assert values['_OVERLAY_MD32X_BSS_END'] < values['__RAM_EMU_END__']
    return values, sizes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.relative_to(ROOT)
    out.mkdir(parents=True, exist_ok=False)
    result = dict(status='preparing', hypothesis='S03', device_access=False,
                  automatic_retry=False, base='PCM-r2 + validated S01 single-commit',
                  flags=FLAGS, image=IMAGE, physical_fps_measured=False)
    save(out / 'state.json', result)
    try:
        assert digest(S01) == '325b2f6de8ca9f862521ef47ea0b1002e07034ea9c89395e47b7659267dc816b'
        assert digest(NORMAL / 'external/picodrive/cpu/sh2/mame/sh2pico.c') == 'ce3921dbe4a8fb8814d7915001ff1af48cce9b9a4f54d029001fe91fb50e1e47'
        result['normal_baseline_elf_sha256'] = digest(NORMAL / 'build/gw_retro_go.elf')
        pd = NORMAL / 'external/picodrive'
        # The legacy CZ80 comments are byte-encoded, not UTF-8. Preserve
        # them and the included opcode files' CRLF in the source/patch.
        original = (pd / 'cpu/cz80/cz80.c').read_bytes().decode('latin-1')
        flag_loop = original.split('// flags tables initialisation\n', 1)[1].split('#if CZ80_BIG_FLAGS_ARRAY', 1)[0]
        proof = '''#include <assert.h>
#include <stdio.h>
typedef unsigned char UINT8;
enum {CF=1,NF=2,PF=4,VF=4,XF=8,HF=16,YF=32,ZF=64,SF=128};
static UINT8 SZ[256],SZP[256],SZ_BIT[256],SZHV_inc[256],SZHV_dec[256];
int main(void) {
unsigned i,j,p;
''' + flag_loop + '''
unsigned long cases=0;
for(unsigned src=0;src<256;src++) for(unsigned bit=0;bit<8;bit++)
for(unsigned carry=0;carry<2;carry++) {
  unsigned idx=src & (1u<<bit), a=SZ_BIT[idx], b=SZP[idx];
  assert(a==b);
  assert((((carry|HF|a)&~(XF|YF))|(src&(XF|YF))) ==
         (((carry|HF|b)&~(XF|YF))|(src&(XF|YF)))); cases++;
  assert((((carry|HF|a)&~(XF|YF))|(0xc0&(XF|YF))) ==
         (((carry|HF|b)&~(XF|YF))|(0xc0&(XF|YF)))); cases++;
  for(unsigned addr_hi=0;addr_hi<256;addr_hi++) {
    assert((carry|HF|(a&~(YF|XF))|(addr_hi&(YF|XF))) ==
           (carry|HF|(b&~(YF|XF))|(addr_hi&(YF|XF)))); cases++;
  }
}
printf("BIT_TABLE_PROOF_PASS cases=%lu\\n",cases);
return 0;
}
'''
        (out / 'bit-table-proof.c').write_text(proof)
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(out / 'bit-table-proof.c'),
                        '-o', str(out / 'bit-table-proof')], check=True, timeout=30)
        proof_output = subprocess.check_output([str(out / 'bit-table-proof')], text=True, timeout=30)
        assert proof_output == 'BIT_TABLE_PROOF_PASS cases=1056768\n', proof_output
        (out / 'proof.log').write_text(proof_output)
        result['proof'] = dict(status='passed', cases=1056768,
                             scope='All byte inputs, eight BIT indices, carry states and indexed address high bytes; original C table initialization')
        sources = {}
        patches = []
        for name, count in [('cz80.c', 3), ('cz80_opCB.c', 2), ('cz80_opXYCB.c', 1)]:
            relative = Path('cpu/cz80') / name
            old = (pd / relative).read_bytes()
            assert old.count(b'SZ_BIT') == count
            if name == 'cz80.c':
                new = old.replace(b'static UINT8 ALIGN_DATA SZ_BIT[256];\n', b'')
                new = new.replace(b'\t\tSZ_BIT[i] = i & (SF | YF | XF);\n\t\tif (!i) SZ_BIT[i] |= ZF | PF;\n\n', b'')
                assert b'SZ_BIT' not in new
                anchor = b'static UINT8 ALIGN_DATA SZP[256];\n'
                new = new.replace(anchor, anchor + b'/* BIT handlers index only 0 or one set bit: SZP equals the old\n * BIT-specific flags there. Share this table without extra hot-path work. */\n')
            else:
                assert old.count(b'& (1 << ((Opcode >> 3) & 7))]') == count
                new = old.replace(b'SZ_BIT[', b'SZP[')
            assert new != old
            sources[relative] = new
            patches.extend(difflib.unified_diff(old.decode('latin-1').splitlines(True), new.decode('latin-1').splitlines(True),
                fromfile='a/' + str(relative), tofile='b/' + str(relative)))
        (out / 's03-cz80-bit-table.patch').write_bytes(''.join(patches).encode('latin-1'))
        w = out / 'firmware'
        subprocess.run(['cp', '-a', '--reflink=auto', str(NORMAL), str(w)], check=True, timeout=180)
        for relative, data in sources.items():
            (w / 'external/picodrive' / relative).write_bytes(data)
        (w / 'external/picodrive/cpu/sh2/mame/sh2pico.c').write_bytes(S01.read_bytes())
        result['source_sha256'] = {str(p): digest(w / 'external/picodrive' / p) for p in sources}
        # Explicitly recompile the two changed objects; keep normal-release
        # flags and source. The copied baseline has no benchmark macros.
        docker = ['docker', 'run', '--rm', '--init', '--user', '1000:1000',
                  '-e', 'GIT_TAG_OVERRIDE=testbed-full-20261005-pcm-r2',
                  '-v', str(w) + ':/opt/workdir', IMAGE]
        result['compiler'] = subprocess.check_output(docker + ['arm-none-eabi-gcc', '--version'], text=True).splitlines()[0]
        with (out / 'build.log').open('w') as log:
            subprocess.run(docker + ['make', '-j3', 'all', *FLAGS], stdout=log,
                           stderr=subprocess.STDOUT, check=True, timeout=1800)
        elf = w / 'build/gw_retro_go.elf'
        values, sizes = inspect_completed_build(w, out)
        oldsymbols = subprocess.check_output(['arm-none-eabi-nm', '-S', str(NORMAL / 'build/gw_retro_go.elf')], text=True)
        oldsz = next(int(l.split()[1], 16) for l in oldsymbols.splitlines() if l.split()[-1] == 'md32x__Cz80_Exec')
        result['placement'] = {key: hex(values[key]) for key in (
            '__md32x_itc_end__', '__md32x_itc_bss_end__', '_OVERLAY_MD32X_BSS_END', '__RAM_EMU_END__')}
        result.update(status='normal_firmware_link_passed',
                      itcm_free_bytes=0x10000-values['__md32x_itc_bss_end__'],
                      ram_emu_free_bytes=values['__RAM_EMU_END__']-values['_OVERLAY_MD32X_BSS_END'],
                      cz80_exec_bytes=dict(baseline=oldsz, candidate=sizes['md32x__Cz80_Exec']),
                      table_bss_removed_bytes=256,
                      normal_benchmark_symbols_absent=True,
                      candidate_elf_sha256=digest(elf),
                      release_tag_reused_for_local_fit_only=True,
                      candidate_installed=False,
                      limitation='No new physical performance claim; normal artifact is an isolated local fit build, not a published release')
    except Exception as error:
        result.update(status='failed', error=str(error), automatic_retry=False)
        (out / 'error.log').write_text(traceback.format_exc())
    save(out / 'result.json', result)
    save(out / 'state.json', result)
    print(json.dumps(result, ensure_ascii=False), flush=True)
    if result['status'] == 'failed':
        raise SystemExit(2)


if __name__ == '__main__':
    main()
