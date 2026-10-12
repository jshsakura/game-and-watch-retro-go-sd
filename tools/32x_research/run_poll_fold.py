#!/usr/bin/env python3
"""Isolated S01 BT/S constant-work candidate, full-helper proof and clean rig.

Local GCC is used for both baseline and candidate. Reuse immutable common
objects and ROM objects. No device, retry, status polling or ROM duplication.
"""
import argparse
import difflib
import json
from pathlib import Path
import re
import shutil
import subprocess

import run_aot_blocks as rig
from build_fixed_thumb_device import IMAGE

ROOT = rig.ROOT
PRIOR = ROOT / 'build/32x-fixed-thumb-v4-20261007'
SOURCE_HASH = 'ce3921dbe4a8fb8814d7915001ff1af48cce9b9a4f54d029001fe91fb50e1e47'
SOURCE = Path('cpu/sh2/mame/sh2pico.c')


def save(p, v):
    p.write_text(json.dumps(v, ensure_ascii=False, indent=2) + '\n')


def replacement(original, compact=False, single_commit=False):
    if single_commit:
        old = '''\t\t\t\twhile (sh2->icount >= 5) {
\t\t\t\t\tUINT32 masked = sh2->r[dest] & mask_v;\t/* delay slot */
\t\t\t\t\tint t = (masked == ref_v);\t\t/* head CMP/EQ */
\t\t\t\t\tsh2->r[dest] = (UINT32)RL(sh2, pa);\t/* reload */
\t\t\t\t\tsh2->t_flag = t ? T : 0;
\t\t\t\t\tsh2->icount -= 5;
\t\t\t\t\tif (!t)
\t\t\t\t\t\treturn;\t/* real BT/S dispatches untaken */
\t\t\t\t}
'''
        assert original.count(old) == 1
        new = '''#if !defined(GNW_NO_INLINE_DATA_READ)
                /* Direct aligned SDRAM has no writer or callback inside
                 * this helper. Calculate both possible comparisons locally
                 * and commit dest/T/budget once, preserving early exit and
                 * every complete five-cycle iteration within this slice. */
                int remaining = sh2->icount;
                if (remaining >= 5) {
                    int t = ((sh2->r[dest] & mask_v) == ref_v);
                    UINT32 raw = (UINT32)RL(sh2, pa);
                    if (t && remaining >= 10) {
                        t = ((raw & mask_v) == ref_v);
                        if (t) remaining %= 5;
                        else   remaining -= 10;
                    }
                    else remaining -= 5;
                    sh2->r[dest] = raw;
                    sh2->t_flag = t ? T : 0;
                    sh2->icount = remaining;
                }
#else
''' + old + '#endif\n'
        return original.replace(old, new)
    if compact:
        anchor = '\t\t\t\t\tif (!t)\n\t\t\t\t\t\treturn;\t/* real BT/S dispatches untaken */\n'
        assert original.count(anchor) == 1
        block = '''#if !defined(GNW_NO_INLINE_DATA_READ)
                    /* RL above directly read an aligned SDRAM array slot.
                     * No writer, CPU sync or event runs inside this helper.
                     * The first comparison used the old dest. All remaining
                     * comparisons use the same raw reload and fixed mask.
                     * A false next comparison still consumes five cycles. */
                    if (sh2->icount >= 5) {
                        t = ((sh2->r[dest] & mask_v) == ref_v);
                        sh2->t_flag = t ? T : 0;
                        if (t) sh2->icount %= 5;
                        else   sh2->icount -= 5;
                    }
                    return;
#endif
'''
        return original.replace(anchor, anchor + block)
    anchor = '\t\t\t\tUINT32 ref_v  = sh2->r[ref];\n'
    assert original.count(anchor) == 1
    block = '''#if !defined(GNW_NO_INLINE_DATA_READ)
                /* The aligned SDRAM guard above selects RL's direct array
                 * load. No writer, event delivery or CPU sync can run inside
                 * this helper. Retain the first comparison of the old dest;
                 * subsequent comparisons all use the same reloaded value.
                 * Preserve the next false comparison's five-cycle charge. */
                if (sh2->icount >= 5 && sh2->p_sdram) {
                    int t = ((sh2->r[dest] & mask_v) == ref_v);
                    sh2->r[dest] = (UINT32)RL(sh2, pa);
                    sh2->t_flag = t ? T : 0;
                    sh2->icount -= 5;
                    if (!t) return;
                    if (sh2->icount >= 5) {
                        t = ((sh2->r[dest] & mask_v) == ref_v);
                        sh2->t_flag = t ? T : 0;
                        if (t) sh2->icount %= 5;
                        else   sh2->icount -= 5;
                    }
                    return;
                }
#endif
'''
    return original.replace(anchor, anchor + block)


def proof_source(original, candidate):
    anchor = 'static void gnw_sh2_fastloop(SH2 *sh2, UINT32 opcode)\n{'
    start = original.index(anchor)
    pos = start + len(anchor)
    depth = 1
    while depth:
        depth += (original[pos] == '{') - (original[pos] == '}')
        pos += 1
    reference = original[start:pos].replace('gnw_sh2_fastloop(', 'poll_reference_fastloop(', 1)
    candidate = candidate.replace(anchor, reference + '\n\n' + anchor, 1)
    # Both helpers are now available; the next block is outside the function.
    after = original[pos:pos + 120]
    assert after in candidate
    return candidate.replace(after, '\n#include "poll_fold_proof.h"\n' + after, 1)


def build(out, title, arm, compiler='local'):
    prior = PRIOR / f'{title}-baseline-clean'
    meta = json.loads((prior / 'inputs.json').read_text())
    assert rig.digest(prior / 'rig_32x.elf') == meta['elf_sha256']
    assert rig.digest(Path(meta['rom'])) == meta['rom_sha256']
    case = out / f'{title}-{arm}'
    case.mkdir()
    for obj in prior.glob('*.o'):
        shutil.copyfile(obj, case / obj.name)
    defs = meta['defs']
    source = out / arm
    harness = out / ('rig_proof.c' if arm == 'proof' else 'rig_clean.c')
    with (case / 'build.log').open('w') as log:
        def cc(argv):
            prefix = (['docker', 'run', '--rm', '--init', '--user', '1000:1000',
                       '-v', str(ROOT) + ':/opt/workdir', IMAGE, 'arm-none-eabi-gcc']
                      if compiler == 'docker' else ['arm-none-eabi-gcc'])
            subprocess.run([*prefix, *rig.ARCH, *rig.FLAGS, *argv],
                           cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=180)
        cc([*defs, '-I' + rig.rel(source), '-I' + rig.rel(out),
            '-c', rig.rel(source / SOURCE), '-o', rig.rel(case / 'cpu_sh2_mame_sh2pico.o')])
        cc([*defs, '-I' + rig.rel(source), '-I' + rig.rel(source / 'pico'),
            '-I' + rig.rel(PRIOR / 'rig'), '-c', rig.rel(harness),
            '-o', rig.rel(case / 'rig_32x.o')])
        cc(['-T' + rig.rel(PRIOR / 'rig/mps2_an500_32x.ld'), '-nostartfiles',
            '-Wl,--gc-sections', '-Wl,-Map=' + rig.rel(case / 'rig.map'),
            *[rig.rel(p) for p in sorted(case.glob('*.o'))], '-lm', '-lc',
            '-o', rig.rel(case / 'rig_32x.elf')])
    version_cmd = (['docker', 'run', '--rm', IMAGE, 'arm-none-eabi-gcc', '--version']
                   if compiler == 'docker' else ['arm-none-eabi-gcc', '--version'])
    meta.update(elf_sha256=rig.digest(case / 'rig_32x.elf'), arm=arm,
                compiler=subprocess.check_output(version_cmd, text=True).splitlines()[0],
                common_objects_origin=str(prior), cpu_source_sha256=rig.digest(source / SOURCE),
                harness_sha256=rig.digest(harness), guest_observer=False,
                diagnostic=arm == 'proof', physical_fps_measured=False)
    save(case / 'inputs.json', meta)
    return case


def run(case, timeout, proof=False):
    meta = json.loads((case / 'inputs.json').read_text())
    assert rig.digest(case / 'rig_32x.elf') == meta['elf_sha256']
    cmd = ['qemu-system-arm', '-machine', 'mps2-an500', '-nographic', '-semihosting']
    if not proof:
        cmd += ['-icount', 'shift=0,align=off,sleep=off']
    cmd += ['-kernel', str(case / 'rig_32x.elf')]
    with (case / 'qemu.log').open('w') as log:
        subprocess.run(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=timeout)
    text = (case / 'qemu.log').read_text()
    if proof:
        m = re.search(r'POLL_FOLD_PROOF_PASS cases=(\d+) context_bytes=(\d+) reject_cache_equal=1', text)
        assert m, 'Proof incomplete'
        result = dict(status='completed_full_helper_proof', cases=int(m[1]),
                      context_bytes=int(m[2]), rejection_cache_equal=True,
                      limitations='Synthetic direct-SDRAM helper states, not all scheduler or memory-mapped states')
    else:
        m = re.search(r'done (\d+) frames  avg host=(\d+) .*?avg sh2=(\d+)', text)
        assert m and int(m[1]) == meta['window'][1] and 'GATE3 PASS' in text
        old = (Path(meta['common_objects_origin']) / 'qemu.log').read_text()
        sig = lambda s: [l for l in s.splitlines() if any(k in l for k in ('snd hash=', 'snd energy', 'fb f'))]
        dumps = lambda s: re.findall(r'\[fbd\] frame=.*?\[fbd\] end', s, re.S)
        assert sig(text) == sig(old)
        assert len(dumps(text)) == 2 and dumps(text) == dumps(old)
        result = dict(status='completed_equal_clean_rig', window=meta['window'],
                      host_insns_per_frame=int(m[2]), framebuffer_endpoints_equal=True,
                      audio_signatures_equal=True, physical_fps_measured=False)
    save(case / 'result.json', result)
    print(json.dumps({'case': case.name, **result}), flush=True)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--phase', choices=['prepare-proof', 'after-burner', 'doom'], required=True)
    p.add_argument('--compact', action='store_true', help='Eliminate duplicated original loop in inline-read builds')
    p.add_argument('--single-commit', action='store_true', help='Compute both comparisons locally and commit CPU state once')
    p.add_argument('--compiler', choices=['local', 'docker'], default='local')
    p.add_argument('--reuse-baseline', type=Path, help='Reuse an unchanged completed baseline from a prior candidate')
    p.add_argument('--timeout', type=int, default=600)
    args = p.parse_args()
    out = args.output.resolve()
    out.relative_to(ROOT)
    if args.phase == 'prepare-proof':
        out.mkdir(parents=True, exist_ok=False)
        state = dict(status='preparing', hypothesis='S01', device_access=False,
                     census='build/32x-poll-census-20261008/run-local-000001/result.json',
                     baseline_source_sha256=SOURCE_HASH, compiler_mode=args.compiler, automatic_retry=False)
        save(out / 'state.json', state)
        try:
            original = (PRIOR / 'baseline' / SOURCE).read_text()
            assert rig.digest(PRIOR / 'baseline' / SOURCE) == SOURCE_HASH
            candidate = replacement(original, compact=args.compact, single_commit=args.single_commit)
            state['candidate_variant'] = ('single-commit' if args.single_commit else
                                          'compact' if args.compact else 'guarded-duplicate')
            for name in ['baseline', 'candidate', 'proof']:
                shutil.copytree(PRIOR / 'baseline', out / name)
            (out / 'candidate' / SOURCE).write_text(candidate)
            (out / 'proof' / SOURCE).write_text(proof_source(original, candidate))
            (out / 'candidate.patch').write_text(''.join(difflib.unified_diff(original.splitlines(True),
                candidate.splitlines(True), fromfile='a/' + str(SOURCE), tofile='b/' + str(SOURCE))))
            shutil.copyfile(ROOT / 'tools/32x_research/poll_fold_proof.h', out / 'poll_fold_proof.h')
            harness = (PRIOR / 'rig/rig_32x_clean.c').read_text()
            (out / 'rig_clean.c').write_text(harness)
            harness = harness.replace('    setvbuf(stdout, 0, _IONBF, 0);',
                '    setvbuf(stdout, 0, _IONBF, 0);\n    extern void poll_fold_proof(void);\n    poll_fold_proof();\n    return 0;', 1)
            (out / 'rig_proof.c').write_text(harness)
            proof = run(build(out, 'doom', 'proof', args.compiler), args.timeout, proof=True)
            for title in ['after-burner', 'doom']:
                for arm in ['baseline', 'candidate']:
                    build(out, title, arm, args.compiler)
            state.update(status='proof_pass_clean_arms_built', proof=proof,
                         runner_sha256=rig.digest(Path(__file__)),
                         candidate_sha256=rig.digest(out / 'candidate' / SOURCE))
            save(out / 'state.json', state)
            print(json.dumps(state), flush=True)
        except Exception as error:
            state.update(status='failed', error_type=type(error).__name__, error=str(error))
            save(out / 'state.json', state)
            raise
    else:
        state = json.loads((out / 'state.json').read_text())
        assert state['status'] == 'proof_pass_clean_arms_built'
        result_path = out / f'{args.phase}-comparison.json'
        assert not result_path.exists(), 'Completed evidence must be reused'
        try:
            baseline_case = out / f'{args.phase}-baseline'
            if args.reuse_baseline:
                baseline_case = args.reuse_baseline.resolve() / f'{args.phase}-baseline'
                baseline_case.relative_to(ROOT)
                old_meta = json.loads((baseline_case / 'inputs.json').read_text())
                new_meta = json.loads((out / f'{args.phase}-baseline/inputs.json').read_text())
                for key in ['rom_sha256', 'window', 'defs', 'compiler', 'common_objects_origin',
                            'cpu_source_sha256', 'harness_sha256', 'guest_observer', 'diagnostic']:
                    assert old_meta[key] == new_meta[key], f'Baseline input mismatch: {key}'
                assert rig.digest(baseline_case / 'rig_32x.elf') == old_meta['elf_sha256']
                a = json.loads((baseline_case / 'result.json').read_text())
                assert a['status'] == 'completed_equal_clean_rig'
            else:
                a = run(baseline_case, args.timeout)
            b = run(out / f'{args.phase}-candidate', args.timeout)
            result = dict(status='completed_clean_comparison', baseline=a, candidate=b,
                          baseline_evidence=str(baseline_case / 'result.json'),
                          baseline_reused=bool(args.reuse_baseline),
                          host_instruction_change_pct=100 * (b['host_insns_per_frame'] / a['host_insns_per_frame'] - 1),
                          device_access=False, physical_fps_measured=False)
            save(result_path, result)
            print(json.dumps(result), flush=True)
        except Exception as error:
            save(result_path, dict(status='failed', error_type=type(error).__name__,
                                   error=str(error), automatic_retry=False))
            raise


if __name__ == '__main__':
    main()
