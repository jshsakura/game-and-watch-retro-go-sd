#!/usr/bin/env python3
"""Resumeable whole-corpus A/B gate for the dormant SNES DSP echo fast path."""
from __future__ import annotations

import argparse
import csv
import hashlib
import os
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import statistics

import run_snes_cost_batch as batch
import run_snes_ppu_deep as deep

ROOT = Path(__file__).resolve().parents[2]
RIG = ROOT / "tools/m7_qemu_rig"
FIELDS = [
    "rom", "rom_sha256", "build_id", "frames", "status", "statehash",
    "audiohash", "corehash", "baseline_emu", "candidate_emu",
    "baseline_audio", "candidate_audio", "baseline_dsp", "candidate_dsp",
    "emu_saved", "audio_saved", "dsp_saved", "total_saved", "saved_pct",
    "active_voices", "echo_voices", "echo_write_frames", "error",
]


def profile(rom: Path, digest: str, build_id: str, frames: int,
            timeout: int) -> dict[str, object]:
    row: dict[str, object] = {field: "" for field in FIELDS}
    row.update(rom=os.path.relpath(rom, ROOT), rom_sha256=digest,
               build_id=build_id, frames=frames)
    try:
        if batch.needs_coprocessor(rom):
            row.update(status="UNSUPPORTED",
                       error="coprocessor/mapper rejected by device preflight")
            return row
        baseline, _ = batch.run_elf(
            RIG / "build_cost/snes_dsp_baseline.elf", rom, timeout)
        candidate, _ = batch.run_elf(
            RIG / "build_cost/snes_cost_on.elf", rom, timeout)
        for key in ("state", "audiohash", "corehash"):
            if baseline[key] != candidate[key]:
                raise RuntimeError(
                    f"{key} mismatch {baseline[key]} != {candidate[key]}")
        emu_saved = int(baseline["emu"]) - int(candidate["emu"])
        audio_saved = int(baseline["audio"]) - int(candidate["audio"])
        dsp_saved = int(baseline["dsp"]) - int(candidate["dsp"])
        total_saved = emu_saved + audio_saved
        baseline_total = int(baseline["emu"]) + int(baseline["audio"])
        row.update(
            status="PASS", statehash=candidate["state"],
            audiohash=candidate["audiohash"], corehash=candidate["corehash"],
            baseline_emu=baseline["emu"], candidate_emu=candidate["emu"],
            baseline_audio=baseline["audio"], candidate_audio=candidate["audio"],
            baseline_dsp=baseline["dsp"], candidate_dsp=candidate["dsp"],
            emu_saved=emu_saved, audio_saved=audio_saved, dsp_saved=dsp_saved,
            total_saved=total_saved,
            saved_pct=f"{100.0 * total_saved / max(1, baseline_total):.4f}",
            active_voices=f'{int(candidate["active"])/1000:.3f}',
            echo_voices=f'{int(candidate["echo"])/1000:.3f}',
            echo_write_frames=candidate["echo_frames"])
    except Exception as exc:
        row.update(status="FAIL", error=str(exc))
    return row


def summary(rows: list[dict[str, str]]) -> None:
    counts = {status: sum(row["status"] == status for row in rows)
              for status in ("PASS", "UNSUPPORTED", "FAIL")}
    savings = [int(row["total_saved"]) for row in rows if row["status"] == "PASS"]
    if not savings:
        print(f"SUMMARY {counts}")
        return
    positive = sum(value > 0 for value in savings)
    zero = sum(value == 0 for value in savings)
    negative = sum(value < 0 for value in savings)
    ordered = sorted(savings)
    p95 = ordered[min(len(ordered) - 1, int(len(ordered) * .95))]
    print(f"SUMMARY pass={counts['PASS']} unsupported={counts['UNSUPPORTED']} "
          f"fail={counts['FAIL']} positive/zero/negative={positive}/{zero}/{negative} "
          f"saved mean={statistics.fmean(savings):.0f} median={statistics.median(savings):.0f} "
          f"p95={p95} insn/frame")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--frames", type=int, default=300)
    parser.add_argument("--jobs", type=int, default=max(1, min(4, os.cpu_count() or 1)))
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--rerun", action="store_true")
    args = parser.parse_args()
    deep.ensure_build(args.frames)
    candidate = RIG / "build_cost/snes_cost_on.elf"
    baseline = RIG / "build_cost/snes_dsp_baseline.elf"
    build_id = hashlib.sha256(candidate.read_bytes() + baseline.read_bytes()).hexdigest()
    roms = batch.roms_under(args.paths)
    if not roms:
        parser.error("no .smc/.sfc ROMs found")
    done: dict[str, tuple[str, str, str]] = {}
    existing: list[dict[str, str]] = []
    if args.csv.exists() and not args.rerun:
        with args.csv.open(newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream)
            if reader.fieldnames != FIELDS:
                parser.error("unknown CSV schema; use --rerun")
            existing = list(reader)
        done = {row["rom"]: (row["rom_sha256"], row["build_id"], row["frames"])
                for row in existing if row["status"] in {"PASS", "UNSUPPORTED"}}
    indexed = [(rom, batch.sha256(rom)) for rom in roms]
    pending = [(rom, digest) for rom, digest in indexed
               if done.get(os.path.relpath(rom, ROOT)) !=
               (digest, build_id, str(args.frames))]
    args.csv.parent.mkdir(parents=True, exist_ok=True)
    mode = "w" if args.rerun or not args.csv.exists() else "a"
    print(f"SNES DSP A/B: {len(roms)} found, {len(roms)-len(pending)} current, "
          f"{len(pending)} pending")
    fresh: list[dict[str, object]] = []
    with args.csv.open(mode, newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        if mode == "w":
            writer.writeheader()
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            futures = {pool.submit(profile, rom, digest, build_id, args.frames,
                                   args.timeout): rom for rom, digest in pending}
            for index, future in enumerate(as_completed(futures), 1):
                row = future.result()
                fresh.append(row)
                writer.writerow(row)
                stream.flush()
                print(f"[{index}/{len(pending)}] {row['status']:11} {row['rom']} "
                      f"saved={row['total_saved'] or '-'}")
    all_rows = existing + [{key: str(value) for key, value in row.items()}
                           for row in fresh]
    summary(all_rows)
    return 1 if any(row["status"] == "FAIL" for row in fresh) else 0


if __name__ == "__main__":
    raise SystemExit(main())
