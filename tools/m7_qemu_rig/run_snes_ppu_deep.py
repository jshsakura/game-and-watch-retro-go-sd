#!/usr/bin/env python3
"""Deep PPU stage attribution on the reusable M7 SNES rig."""
from __future__ import annotations

import argparse
import csv
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import re
import subprocess

import run_snes_cost_batch as batch

ROOT = Path(__file__).resolve().parents[2]
RIG = ROOT / "tools/m7_qemu_rig"
DEEP_RE = re.compile(
    r"\[ppu-deep\] bg1=(\d+) bg2=(\d+) bg3=(\d+) "
    r"sprite_eval=(\d+) sprite_draw=(\d+) clear=(\d+) palette=(\d+) "
    r"fast=(\d+) math=(\d+) linecopy=(\d+) fast_pixels=(\d+) "
    r"math_pixels=(\d+)")
FIELDS = ["rom", "frames", "ppu_total", "bg1", "bg2", "bg3",
          "sprite_eval", "sprite_draw", "clear", "palette", "fast", "math",
          "linecopy", "residual", "fast_pixels", "math_pixels", "status", "error"]


def ensure_build(frames: int) -> None:
    stamp = RIG / "build_cost/frames.txt"
    elfs = [RIG / "build_cost/snes_cost_on.elf",
            RIG / "build_cost/snes_cost_off.elf",
            RIG / "build_cost/snes_ppu_deep.elf"]
    deps = [RIG / "build_snes_cost.sh", RIG / "rig_snes.c",
            RIG / "rig_runtime_hf.c", RIG / "mps2_an500_snes.ld"]
    deps += list((ROOT / "external/sm/src/snes").glob("*.[ch]"))
    stale = (not all(path.exists() for path in elfs) or
             max(path.stat().st_mtime for path in deps) >
             min(path.stat().st_mtime for path in elfs))
    current = stamp.read_text().strip() if stamp.exists() else ""
    if current != str(frames) or stale:
        subprocess.run([str(RIG / "build_snes_cost.sh"), str(frames)],
                       cwd=ROOT, check=True)


def profile(rom: Path, frames: int, timeout: int) -> dict[str, object]:
    row: dict[str, object] = {field: "" for field in FIELDS}
    row.update(rom=str(rom), frames=frames)
    try:
        on, _ = batch.run_elf(RIG / "build_cost/snes_cost_on.elf", rom, timeout)
        off, _ = batch.run_elf(RIG / "build_cost/snes_cost_off.elf", rom, timeout)
        deep, log = batch.run_elf(RIG / "build_cost/snes_ppu_deep.elf", rom, timeout)
        for candidate in (off, deep):
            if (candidate["corehash"] != on["corehash"] or
                    candidate["audiohash"] != on["audiohash"]):
                raise RuntimeError("baseline/off/deep COREHASH or AUDIOHASH mismatch")
        match = DEEP_RE.search(log)
        if not match:
            raise RuntimeError("deep ELF did not print stage counters")
        keys = ("bg1", "bg2", "bg3", "sprite_eval", "sprite_draw", "clear",
                "palette", "fast", "math", "linecopy", "fast_pixels", "math_pixels")
        values = dict(zip(keys, map(int, match.groups())))
        ppu = max(0, int(on["emu"]) - int(off["emu"]))
        accounted = sum(values[key] for key in
                        ("bg1", "bg2", "bg3", "sprite_draw", "clear",
                         "palette", "fast", "math", "linecopy"))
        row.update(values)
        row.update(ppu_total=ppu, residual=max(0, ppu - accounted), status="PASS")
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
    ensure_build(args.frames)
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        rows = list(pool.map(
            lambda rom: profile(rom.resolve(), args.frames, args.timeout), args.roms))
    if args.csv:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        with args.csv.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=FIELDS)
            writer.writeheader(); writer.writerows(rows)
    for row in rows:
        if row["status"] != "PASS":
            print(f"FAIL {row['rom']}: {row['error']}")
            continue
        print(f"PASS {row['rom']}: PPU={row['ppu_total']} "
              f"BG={row['bg1']}/{row['bg2']}/{row['bg3']} "
              f"SPR={row['sprite_draw']} FAST={row['fast']} MATH={row['math']} "
              f"LINE={row['linecopy']} RESID={row['residual']} "
              f"PIX={row['fast_pixels']}/{row['math_pixels']}")
    return 1 if any(row["status"] != "PASS" for row in rows) else 0


if __name__ == "__main__":
    raise SystemExit(main())
