#!/usr/bin/env python3
"""Attribute SNES DSP work on the reusable Cortex-M7 QEMU rig."""
from __future__ import annotations

import argparse
import csv
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import re

import run_snes_ppu_deep as ppu_deep
import run_snes_cost_batch as batch

ROOT = Path(__file__).resolve().parents[2]
RIG = ROOT / "tools/m7_qemu_rig"
DEEP_RE = re.compile(
    r"\[dsp-deep\] channels=(\d+) mix=(\d+) echo=(\d+) "
    r"noise=(\d+) store=(\d+)")
FIELDS = ["rom", "frames", "dsp_total", "channels", "mix", "echo",
          "noise", "store", "residual", "active_voices", "echo_voices",
          "echo_write_frames", "statehash", "audiohash", "status", "error"]


def profile(rom: Path, frames: int, timeout: int) -> dict[str, object]:
    row: dict[str, object] = {field: "" for field in FIELDS}
    row.update(rom=str(rom), frames=frames)
    try:
        base, _ = batch.run_elf(RIG / "build_cost/snes_cost_on.elf", rom, timeout)
        deep, log = batch.run_elf(RIG / "build_cost/snes_dsp_deep.elf", rom, timeout)
        if (deep["state"] != base["state"] or
                deep["corehash"] != base["corehash"] or
                deep["audiohash"] != base["audiohash"]):
            raise RuntimeError("baseline/deep state, core or audio hash mismatch")
        match = DEEP_RE.search(log)
        if not match:
            raise RuntimeError("deep ELF did not print DSP stage counters")
        keys = ("channels", "mix", "echo", "noise", "store")
        values = dict(zip(keys, map(int, match.groups())))
        accounted = sum(values.values())
        row.update(values)
        row.update(
            dsp_total=deep["dsp"], residual=max(0, int(deep["dsp"]) - accounted),
            active_voices=f'{int(deep["active"])/1000:.3f}',
            echo_voices=f'{int(deep["echo"])/1000:.3f}',
            echo_write_frames=deep["echo_frames"], statehash=deep["state"],
            audiohash=deep["audiohash"], status="PASS")
    except Exception as exc:
        row.update(status="FAIL", error=str(exc))
    return row


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("roms", nargs="+", type=Path)
    parser.add_argument("--frames", type=int, default=1200)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--csv", type=Path)
    args = parser.parse_args()
    ppu_deep.ensure_build(args.frames)
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        rows = list(pool.map(
            lambda rom: profile(rom.resolve(), args.frames, args.timeout), args.roms))
    if args.csv:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        with args.csv.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=FIELDS)
            writer.writeheader()
            writer.writerows(rows)
    for row in rows:
        if row["status"] != "PASS":
            print(f"FAIL {row['rom']}: {row['error']}")
            continue
        print(f"PASS {row['rom']}: DSP={row['dsp_total']} "
              f"CH={row['channels']} MIX={row['mix']} ECHO={row['echo']} "
              f"NOISE={row['noise']} STORE={row['store']} RESID={row['residual']} "
              f"VOICES={row['active_voices']}/{row['echo_voices']}")
    return 1 if any(row["status"] != "PASS" for row in rows) else 0


if __name__ == "__main__":
    raise SystemExit(main())
