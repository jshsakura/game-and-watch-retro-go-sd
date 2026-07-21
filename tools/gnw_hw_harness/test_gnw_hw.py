#!/usr/bin/env python3

import json
import tempfile
import unittest
from pathlib import Path

import gnw_hw


HERE = Path(__file__).resolve().parent


class HardwareContractTests(unittest.TestCase):
    def make_profile(self, map_path: Path, directory: Path) -> Path:
        profile = {
            "schema_version": 1,
            "device": {"target": "mario", "sd_hardware": "SPI1"},
            "build": {"map_sha256": gnw_hw.sha256_file(map_path)},
            "runtime": {"dtcm_used_at_emu_init_bytes": 74728},
            "clock_profiles": [
                {"name": "profile2", "core_hz": 312000000, "source": "device-log"}
            ],
        }
        path = directory / "profile.json"
        path.write_text(json.dumps(profile), encoding="utf-8")
        return path

    def test_canonical_contract_matches_golden(self):
        map_path = HERE / "fixtures/canonical.map"
        with tempfile.TemporaryDirectory() as raw:
            profile = self.make_profile(map_path, Path(raw))
            contract = gnw_hw.extract_contract(map_path, profile_path=profile)
        self.assertEqual(contract["memory"]["ram_emu"]["total_bytes"], 741376)
        self.assertEqual(
            contract["memory"]["dtcm_heap"]["effective_free_at_emu_init_bytes"],
            8216,
        )
        self.assertEqual(contract["memory"]["ahb"]["effective_dynamic_free_bytes"], 87904)
        self.assertEqual(contract["memory"]["ram_emu"]["overlays"]["snes"]["total_bytes"], 262144)
        self.assertEqual(
            gnw_hw.verify_expected(contract, HERE / "golden/sd_canonical.json"), []
        )

    def test_runtime_free_is_unknown_without_device_profile(self):
        contract = gnw_hw.extract_contract(HERE / "fixtures/canonical.map")
        self.assertIsNone(
            contract["memory"]["dtcm_heap"]["effective_free_at_emu_init_bytes"]
        )

    def test_stale_profile_is_rejected(self):
        with tempfile.TemporaryDirectory() as raw:
            profile = Path(raw) / "profile.json"
            profile.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "build": {"map_sha256": "0" * 64},
                        "runtime": {"dtcm_used_at_emu_init_bytes": 74728},
                        "clock_profiles": [
                            {"name": "profile2", "core_hz": 312000000, "source": "device-log"}
                        ],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(gnw_hw.ContractError, "stale device profile"):
                gnw_hw.extract_contract(HERE / "fixtures/canonical.map", profile_path=profile)

    def test_old_32x_ahb_layout_reproduces_overflow(self):
        contract = gnw_hw.extract_contract(HERE / "fixtures/pre_32x_fix.map")
        free = gnw_hw.allocation_free(contract, "ahb")
        self.assertEqual(free, 79680)
        self.assertGreater(84 * 1024, free)


if __name__ == "__main__":
    unittest.main(verbosity=2)
