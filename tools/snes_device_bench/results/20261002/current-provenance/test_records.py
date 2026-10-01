import json
from pathlib import Path
import struct
import tempfile
import unittest
from collect import FIELDS, decode
from run_device import compare

class Records(unittest.TestCase):
    def setUp(self):
        self.r=dict.fromkeys(FIELDS,0)
        self.r.update(magic=0x53425844,version=1,warmup=120,window=900,completed=1020,done=1,
            start_ms=0xfffff000,end_ms=0x2b98,start_emu=120,end_emu=1020,start_drawn=100,end_drawn=850,
            start_audio_calls=120,end_audio_calls=1020,start_audio_samples=31920,end_audio_samples=271320,
            state_resumed=1,clock_hz=312000000,start_guest_frame=120,end_guest_frame=1020,audio_buffer_samples=266)
    def decode(self,r):
        return decode(struct.pack('<'+'I'*len(FIELDS),*(r[x] for x in FIELDS)))
    def test_timer_wrap(self):
        self.assertEqual(self.decode(self.r)['elapsed_ms'],15256)
    def test_bad_workloads(self):
        for key,value in [('state_resumed',0),('state_refused',5),('end_guest_frame',1019),
            ('input_or',1),('end_muted_calls',1),('end_audio_samples',0),('done',0),('end_drawn',1100)]:
            with self.subTest(key=key):
                bad=dict(self.r);bad[key]=value
                with self.assertRaises(ValueError):self.decode(bad)
    def test_draw_counter_wrap(self):
        self.r.update(start_drawn=0xfffffff0,end_drawn=734)
        self.assertEqual(self.decode(self.r)['delta_drawn'],750)
    def test_incomplete_record(self):
        with self.assertRaises(ValueError):decode(b'\x00'*4)
    def records(self):
        r=self.decode(self.r)
        r.update(audio_sha256='same',screen_sha256='same')
        return [dict(r) for _ in range(3)]
    def test_refuse_drift(self):
        r=self.records();r[2]['drawn_fps']*=1.03
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(compare(r,Path(d)/'result.json')['status'],'invalid_drift')
    def test_refuse_guest_difference(self):
        r=self.records();r[1]['end_state_crc32']=3
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(compare(r,Path(d)/'result.json')['status'],'correctness_unresolved')
    def test_refuse_wrong_rom(self):
        r=self.records();r[1]['rom_crc32']=3
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(ValueError):compare(r,Path(d)/'result.json')

if __name__=='__main__':unittest.main()
