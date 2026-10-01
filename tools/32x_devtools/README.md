# 32X device session tools (2026-10-01)

What the After Burner / Doom campaign (issue #48, `docs/32X_WORKLOG_20261001.md`) ran on the device.
Session artifacts: paths are hardcoded to `build/32x-mixer-hle-20261001/`, and they import
`tools/32x_device_bench/` (`run_arm.py`, `collect.py`, `compare.py`, `gnw_command.py`), which is
**not committed on this branch** (it lives untracked in another session's checkout). Treat these as
the record of the method, not as a ready-to-run suite.

| File | What it does |
|---|---|
| `placement_runs.py` | backup bank2 / cores / CONFIG / off.sav / selector, run arms in a given order (A,B,A,B,A), record each run's XIP address, optionally require an XIP alignment, restore everything and read it back |
| `collect_place.py` | `collect.py` plus the XIP / ROM / heap addresses; relaxed "drawn >= 1" check (Doom flips the panel more than once per frame in some trees) |
| `run_arm_pc.py` | `run_arm.py` that also accepts the raw PC-sample diagnostic arm |
| `pc_symbolize.py` | raw SysTick PC samples to per-function / per-region profile |
| `opening_capture.py` | boot an arm and photograph the panel continuously (LTDC scanout, CPU never halted) |
| `build-wtmain.py`, `build-doom.py` | Docker builds of bench arms (After Burner and Doom workloads) |
| `selector.doom.txt` | `/snes_bench_index.txt` for Doom |
| `install_release.py` | install a **release** build on the device: bank2 firmware plus the `cores/`, `lang/` and `roms/homebrew/` files that carry its tag. Internal cores must carry exactly the firmware's `GIT_TAG` or the launcher shows "corrupted installation", so replacing only the firmware (or only the 32X core, as the measurement scripts do) is not an install. Backs up everything it overwrites, reads everything back, restores on failure |
| `restore_release.py` | undo `install_release.py` from its backup |

Rules these enforce (see `CLAUDE.md`): an arm proves its identity (screen hash, XIP address), the card and
flash are read back after every restore, and a triplet whose baseline drifts more than 0.5% is invalid.
**The device's `audio_sha256` is not evidence** (all zeros after `audio_stop_playing()`); use the rig's hash.
