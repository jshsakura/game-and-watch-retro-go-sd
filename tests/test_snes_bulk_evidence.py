"""A false PASS would promote an invalid device candidate: test fail-closed gates."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'tools/snes_bulk_wait/compare.py'
VALID = ('w00001 emu=100 apu=20 insn/frame fb=01234567 audio=abcdef01 lit=1\n'
         '[snes-qemu] done 1 frames STATEHASH=12345678 AUDIOHASH=abcdef01 avg emu=100 apu=20 insn/frame\n'
         '[frame] 1 emu=100 apu=20 total=120 fb=01234567 audio=abcdef01\n'
         '[state] bytes=100 hash=0123456789abcdef\n')


class EvidenceTests(unittest.TestCase):
    def compare(self, candidate, bad_request=False):
        with tempfile.TemporaryDirectory() as raw:
            paths = []
            for arm, log in [('A', VALID), ('B', candidate)]:
                p = Path(raw) / arm
                p.mkdir()
                (p / 'output.log').write_text(log)
                request = dict(rom_sha256='different' if arm == 'B' and bad_request else 'rom',
                               save=None, input='none', frames=1, gate='natural', case_config=True,
                               audio_dump=True)
                (p / 'request.json').write_text(json.dumps(request))
                result = dict(completed=True, binary_unchanged=True, input_unchanged=True,
                              case_verified=True, pcm_complete=True, pcm=dict(bytes=532, sha256='pcm'))
                (p / 'result.json').write_text(json.dumps(result))
                paths.append(str(p / 'output.log'))
            return subprocess.run(['python3', str(SCRIPT), *paths], capture_output=True, text=True)

    def test_identical(self):
        self.assertEqual(self.compare(VALID).returncode, 0)

    def test_hidden_state_divergence(self):
        self.assertNotEqual(self.compare(VALID.replace('hash=0123456789abcdef', 'hash=1123456789abcdef')).returncode, 0)

    def test_intermediate_audio_divergence(self):
        self.assertNotEqual(self.compare(VALID.replace('total=120 fb=01234567 audio=abcdef01', 'total=120 fb=01234567 audio=abcdef02')).returncode, 0)

    def test_missing_frame_trace(self):
        self.assertNotEqual(self.compare('\n'.join(x for x in VALID.splitlines() if not x.startswith('[frame]'))).returncode, 0)

    def test_wrong_rom(self):
        self.assertNotEqual(self.compare(VALID, bad_request=True).returncode, 0)

    def test_incomplete_execution(self):
        self.assertNotEqual(self.compare('\n'.join(x for x in VALID.splitlines() if 'done 1 frames' not in x)).returncode, 0)


if __name__ == '__main__':
    unittest.main()
