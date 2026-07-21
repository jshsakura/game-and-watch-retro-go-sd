#!/usr/bin/env python3

import json
import tempfile
import unittest
from pathlib import Path

import timing_oracle


class TimingOracleTests(unittest.TestCase):
    def test_fit_predict_and_profile_binding(self):
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            profile_path = directory / "profile.json"
            profile_path.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "build": {"map_sha256": "a" * 64},
                        "runtime": {"dtcm_used_at_emu_init_bytes": 74728},
                        "clock_profiles": [
                            {"name": "profile2", "core_hz": 312000000, "source": "device-log"}
                        ],
                    }
                ),
                encoding="utf-8",
            )
            calibration = {
                "samples": [
                    {
                        "name": "a",
                        "qemu_units": {"emu": 100, "audio": 20},
                        "device_cycles": {"emu": 110, "audio": 40},
                        "fixed_cycles": 10,
                    },
                    {
                        "name": "b",
                        "qemu_units": {"emu": 200, "audio": 40},
                        "device_cycles": {"emu": 220, "audio": 80},
                        "fixed_cycles": 20,
                    },
                ]
            }
            model = timing_oracle.fit(calibration, profile_path)
            self.assertAlmostEqual(model["coefficients_cycles_per_qemu_unit"]["emu"], 1.1)
            self.assertAlmostEqual(model["coefficients_cycles_per_qemu_unit"]["audio"], 2.0)
            result = timing_oracle.predict(
                model,
                {
                    "name": "c",
                    "clock_profile": "profile2",
                    "qemu_units": {"emu": 150, "audio": 30},
                    "fixed_cycles": 15,
                },
                profile_path,
            )
            self.assertAlmostEqual(result["predicted_cycles_per_frame"], 240)
            self.assertAlmostEqual(result["predicted_fps"], 1300000)
            self.assertIsNotNone(result["fps_range_from_calibration_error"])

            profile_path.write_text(profile_path.read_text() + "\n", encoding="utf-8")
            with self.assertRaisesRegex(timing_oracle.TimingError, "different device profile"):
                timing_oracle.predict(model, {"qemu_units": {"emu": 1, "audio": 1}}, profile_path)


if __name__ == "__main__":
    unittest.main(verbosity=2)
