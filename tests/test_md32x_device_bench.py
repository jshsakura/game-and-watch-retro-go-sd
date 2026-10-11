#!/usr/bin/env python3
"""Host checks of precise frame boundaries and conservative result validation."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
def module(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/'tools/32x_device_bench'/f'{name}.py')
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
collect=module('collect')
import sys
sys.modules['collect']=collect
compare=module('compare')

SOURCE='''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
uint32_t g_common_emu_frames=73, g_common_drawn_frames=21;
static uint32_t now=0xfffffff0u;
uint32_t HAL_GetTick(void) { return now; }
void common_emu_bench_begin(void) {}
#include "Core/Inc/porting/md32x/md32x_device_bench.h"
int main(void) {
 gnw_md32x_bench_init();
 unsigned hits=0;
 for (unsigned i=1;i<=GNW_32X_DEVICE_BENCH_WARMUP+GNW_32X_DEVICE_BENCH_FRAMES;i++) {
  now+=7; g_common_emu_frames++;
  bool drawn=i%2==0; g_common_drawn_frames+=drawn;
  gnw_md32x_bench_audio(735,false);
  bool done=gnw_md32x_bench_tick(drawn,0);
  if (done && i!=GNW_32X_DEVICE_BENCH_WARMUP+GNW_32X_DEVICE_BENCH_FRAMES) return 2;
  hits+=done;
 }
 if (hits!=1 || gnw_md32x_bench_tick(true,0)) return 3;
 gnw_md32x_bench_result.clock_hz=312000000;
 fwrite((const void*)&gnw_md32x_bench_result,sizeof(gnw_md32x_bench_result),1,stdout);
 return 0;
}
'''

class DeviceBenchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reports=[]
        with tempfile.TemporaryDirectory() as d:
            source=Path(d)/'test.c';source.write_text(SOURCE)
            for warmup in [0,3,8]:
                binary=Path(d)/f'case-{warmup}'
                subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(ROOT),f'-DGNW_32X_DEVICE_BENCH_WARMUP={warmup}',
                    '-DGNW_32X_DEVICE_BENCH_FRAMES=5',str(source),'-o',str(binary)],check=True)
                raw=subprocess.check_output([str(binary)])
                cls.reports.append(collect.decode(raw))

    def test_unverified_preflight_cannot_start_batch(self):
        import json
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);out=p/'job'
            plan={'evidence':{'conditions':{'environment':{'device_preflight':'blocked'}}}}
            (p/'plan.json').write_text(json.dumps(plan))
            r=subprocess.run([sys.executable,str(ROOT/'tools/32x_device_bench/batch.py'),
                '--plan',str(p/'plan.json'),'--output',str(out),'--timeout','120'],
                capture_output=True,text=True)
            self.assertEqual(r.returncode,2)
            self.assertIn('preflight incomplete',r.stderr)
            self.assertFalse(out.exists())

    def test_existing_debugger_is_refused_without_termination(self):
        from unittest.mock import patch
        from types import SimpleNamespace
        driver=module('gnw_command')
        with patch.object(driver.subprocess,'run',return_value=SimpleNamespace(returncode=0)) as cmd:
            with self.assertRaises(RuntimeError):driver.refuse_existing('openocd')
            self.assertEqual(cmd.call_count,1)
            self.assertEqual(cmd.call_args.args[0],['pgrep','-x','openocd'])
        with patch.object(driver.subprocess,'run',return_value=SimpleNamespace(returncode=1)):
            driver.refuse_existing('openocd')

    def test_exact_window_zero_warmup_and_timer_wrap(self):
        for warmup,r in zip([0,3,8],self.reports):
            self.assertEqual(r['warmup'],warmup)
            self.assertEqual(r['delta_emu'],5)
            self.assertEqual(r['elapsed_ms'],35)
            self.assertEqual(r['delta_audio_calls'],5)
            self.assertEqual(r['delta_audio_samples'],3675)
            self.assertEqual(r['delta_drawn'],sum(i%2==0 for i in range(warmup+1,warmup+6)))
        self.assertLess(self.reports[0]['end_ms'],self.reports[0]['start_ms'])

    def raw(self,r):
        return struct.pack('<'+'I'*len(collect.FIELDS),*[r[k] for k in collect.FIELDS])

    def test_invalid_completion_input_mute_clock_and_window_rejected(self):
        for field,value in [('done',0),('magic',0),('version',2),('completed',1),
            ('input_or',1),('clock_hz',0),('end_emu',999),
            ('end_audio_calls',0),('end_muted_calls',1)]:
            r=dict(self.reports[1]);r[field]=value
            with self.subTest(field=field), self.assertRaises(ValueError):collect.decode(self.raw(r))
        with self.assertRaises(ValueError):collect.decode(b'bad')

    def triplet(self):
        a=dict(self.reports[1],audio_sha256='audio',screen_sha256='screen',elf_sha256='A')
        b=dict(a,elf_sha256='B',drawn_fps=a['drawn_fps']*1.02)
        c=dict(a)
        return a,b,c

    def test_valid_gain_and_hardware_drift_rejection(self):
        a,b,c=self.triplet()
        self.assertAlmostEqual(compare.compare(a,b,c,0.5)['gain_percent'],2)
        c['drawn_fps']*=1.02
        with self.assertRaises(ValueError):compare.compare(a,b,c,0.5)

    def test_workload_binary_and_endpoint_mismatch_rejected(self):
        for field,value in [('clock_hz',280000000),('fullscreen',1),
            ('last_drawn_frame',0),('audio_sha256','changed'),
            ('screen_sha256','changed'),('elf_sha256','A')]:
            a,b,c=self.triplet();b[field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):compare.compare(a,b,c,0.5)

    def test_raw_report_integrity(self):
        import hashlib,json
        r=dict(self.reports[1])
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)
            (p/'record.bin').write_bytes(self.raw(r))
            for n in ['screen','audio']:
                data=b'\0'*153600 if n=='screen' else b'audio'
                (p/(n+'.bin')).write_bytes(data)
                r[n+'_sha256']=hashlib.sha256(data).hexdigest()
            (p/'report.json').write_text(json.dumps(r))
            self.assertEqual(compare.load(p/'report.json')['window'],5)
            (p/'audio.bin').write_bytes(b'corrupt')
            with self.assertRaises(ValueError):compare.load(p/'report.json')

if __name__=='__main__':unittest.main()
