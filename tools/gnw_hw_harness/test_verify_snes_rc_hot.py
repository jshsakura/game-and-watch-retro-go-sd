#!/usr/bin/env python3

import argparse
import contextlib
import io
import tempfile
import unittest
from pathlib import Path

import verify_snes_rc_hot as gate


class VerifySnesRcHotTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def log(self, name: str, state: str, audio: str) -> Path:
        path = self.root / name
        path.write_text(
            "boot\n"
            f"[snes-qemu] done STATEHASH={state} AUDIOHASH={audio} avg emu=1\n",
            encoding="utf-8",
        )
        return path

    def hash_args(self, *, hot_smw_state: str = "11111111") -> argparse.Namespace:
        return argparse.Namespace(
            baseline_smw_log=self.log("smw.base", "11111111", "aaaaaaaa"),
            hot_smw_log=self.log("smw.hot", hot_smw_state, "aaaaaaaa"),
            baseline_zelda_log=self.log("zelda.base", "22222222", "bbbbbbbb"),
            hot_zelda_log=self.log("zelda.hot", "22222222", "bbbbbbbb"),
        )

    def test_equal_state_and_audio_pass(self):
        with contextlib.redirect_stdout(io.StringIO()):
            gate.check_hashes(self.hash_args())

    def test_state_mismatch_fails(self):
        with self.assertRaisesRegex(gate.GateFailure, "SMW: behavior changed"):
            gate.check_hashes(self.hash_args(hot_smw_state="11111110"))

    def test_linker_assert_with_nested_expressions_passes(self):
        script = self.root / "device.ld"
        script.write_text(
            ".itcm_rc_hot (ORIGIN(ITCMRAM) + 0x4) : {\n"
            "  __rc_hot_start__ = .; *(.itcm_rc_hot*) __rc_hot_end__ = .;\n"
            "  ASSERT(ABSOLUTE(__rc_hot_end__) <= "
            "ORIGIN(ITCMRAM) + LENGTH(ITCMRAM), \"rc hot ITCM overflow\");\n"
            "} > ITCMRAM\n",
            encoding="utf-8",
        )
        gate._check_linker_assert(script)

    def test_missing_linker_assert_fails(self):
        script = self.root / "device.ld"
        script.write_text(".itcm_rc_hot : { *(.itcm_rc_hot*) } > ITCMRAM\n")
        with self.assertRaisesRegex(gate.GateFailure, "no nearby <= ITCM linker ASSERT"):
            gate._check_linker_assert(script)


if __name__ == "__main__":
    unittest.main()
