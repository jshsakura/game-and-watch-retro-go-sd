#!/usr/bin/env python3
"""Run the reusable M7 SNES cost rig over one ROM or a whole library.

Each cartridge gets deterministic render-on and render-off runs.  Their
difference is PPU composition cost; the instrumented core reports 65816, SPC700,
DSP and device-equivalent full-frame present-copy costs.  Results append to CSV,
so a large library run can resume without repeating completed ROMs.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed

ROOT = Path(__file__).resolve().parents[2]
RIG = ROOT / "tools/m7_qemu_rig"
FINAL_RE = re.compile(
    r"STATEHASH=([0-9a-fA-F]+) AUDIOHASH=([0-9a-fA-F]+) "
    r"COREHASH=([0-9a-fA-F]+) avg emu=(\d+) apu=(\d+)")
COST_RE = re.compile(
    r"cpu=(\d+) spc700=(\d+) dsp=(\d+) present=(\d+) "
    r"dsp_samples=(\d+) active_voices_x1000=(\d+) "
    r"echo_voices_x1000=(\d+) echo_write_frames=(\d+)")
FIELDS = [
    "rom", "rom_sha256", "build_id", "frames", "status", "statehash", "audiohash",
    "corehash", "emu", "audio", "cpu65816",
    "spc700", "dsp", "ppu", "present", "skeleton", "dsp_samples",
    "active_voices", "echo_voices", "echo_write_frames", "error",
]


def roms_under(paths: list[Path]) -> list[Path]:
    found: set[Path] = set()
    for path in paths:
        path = path.resolve()
        if path.is_file() and path.suffix.lower() in {".smc", ".sfc"}:
            found.add(path)
        elif path.is_dir():
            for suffix in ("*.smc", "*.sfc", "*.SMC", "*.SFC"):
                found.update(p.resolve() for p in path.rglob(suffix))
    return sorted(found)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def needs_coprocessor(rom: Path) -> bool:
    """Mirror main_snes.c's supported-cart preflight."""
    data = rom.read_bytes()
    if len(data) % 1024 == 512:
        data = data[512:]
    for offset in (0x7FB0, 0xFFB0):
        if offset + 0x30 > len(data):
            continue
        header = data[offset:offset + 0x30]
        checksum = header[0x2E] | header[0x2F] << 8
        complement = header[0x2C] | header[0x2D] << 8
        if checksum ^ complement == 0xFFFF:
            return header[0x26] >= 0x03
    return False


def run_elf(elf: Path, rom: Path, timeout: int) -> tuple[dict[str, int | str], str]:
    size = rom.stat().st_size
    if not 0 < size <= 0x800000:
        raise RuntimeError(f"ROM size {size} is outside rig range 1..8 MiB")
    # QEMU's -device property parser treats commas in paths as separators.  A
    # stable-name symlink also keeps Unicode/space-heavy library names out of the
    # command line without copying multi-megabyte ROMs.
    with tempfile.TemporaryDirectory(prefix="snes-rig-") as raw_tmp:
        tmp = Path(raw_tmp)
        safe_rom = tmp / "rom.sfc"
        safe_rom.symlink_to(rom)
        length = tmp / "length.bin"
        length.write_bytes(struct.pack("<I", size))
        cmd = [
            "qemu-system-arm", "-machine", "mps2-an500", "-nographic",
            "-semihosting", "-icount", "shift=0,align=off,sleep=off",
            "-kernel", str(elf),
            "-device", f"loader,file={safe_rom},addr=0x60800000,force-raw=on",
            "-device", f"loader,file={length},addr=0x607ffffc,force-raw=on",
        ]
        proc = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=timeout)
    final = FINAL_RE.search(proc.stdout)
    cost = COST_RE.search(proc.stdout)
    if proc.returncode or not final or not cost:
        tail = " | ".join(proc.stdout.splitlines()[-5:])
        raise RuntimeError(f"qemu rc={proc.returncode}; {tail}")
    keys = ("cpu", "spc", "dsp", "present", "samples", "active", "echo", "echo_frames")
    values: dict[str, int | str] = {
        "state": final.group(1).lower(), "audiohash": final.group(2).lower(),
        "corehash": final.group(3).lower(),
        "emu": int(final.group(4)), "audio": int(final.group(5)),
    }
    values.update(zip(keys, map(int, cost.groups())))
    return values, proc.stdout


def profile(rom: Path, rom_hash: str, build_id: str, frames: int,
            timeout: int, logs: Path | None) -> dict[str, object]:
    row: dict[str, object] = {key: "" for key in FIELDS}
    row["rom"] = os.path.relpath(rom, ROOT)
    row["rom_sha256"] = rom_hash
    row["build_id"] = build_id
    row["frames"] = frames
    try:
        if needs_coprocessor(rom):
            row.update(status="UNSUPPORTED", error="coprocessor/mapper rejected by device preflight")
            return row
        on, on_log = run_elf(RIG / "build_cost/snes_cost_on.elf", rom, timeout)
        off, off_log = run_elf(RIG / "build_cost/snes_cost_off.elf", rom, timeout)
        if on["corehash"] != off["corehash"] or on["audiohash"] != off["audiohash"]:
            raise RuntimeError(
                "render on/off changed machine state: "
                f"core {on['corehash']} != {off['corehash']} or "
                f"audio {on['audiohash']} != {off['audiohash']}")
        if logs:
            logs.mkdir(parents=True, exist_ok=True)
            safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(row["rom"]))
            (logs / f"{safe}.log").write_text("=== ON ===\n" + on_log + "=== OFF ===\n" + off_log)
        ppu = max(0, int(on["emu"]) - int(off["emu"]))
        # CPU timing contains its bus callbacks, and those can catch the APU up;
        # SPC/DSP are therefore intentionally overlapping drill-down counters.
        # Only subtract CPU from the render-off emulation total for the residual
        # event/DMA/timing skeleton; never subtract all three from one another.
        skeleton = max(0, int(off["emu"]) - int(off["cpu"]))
        row.update({
            "status": "PASS", "statehash": on["state"], "audiohash": on["audiohash"],
            "corehash": on["corehash"],
            "emu": on["emu"], "audio": on["audio"], "cpu65816": on["cpu"],
            "spc700": on["spc"], "dsp": on["dsp"], "ppu": ppu,
            "present": on["present"], "skeleton": skeleton,
            "dsp_samples": on["samples"], "active_voices": f'{int(on["active"])/1000:.3f}',
            "echo_voices": f'{int(on["echo"])/1000:.3f}',
            "echo_write_frames": on["echo_frames"],
        })
    except subprocess.TimeoutExpired:
        row.update(status="TIMEOUT", error=f">{timeout}s")
    except Exception as exc:  # preserve the failure as a row; do not kill a 2200-ROM run
        row.update(status="FAIL", error=str(exc))
    return row


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+", type=Path, help="ROM files or directories")
    ap.add_argument("--csv", type=Path, default=RIG / "snes_costs.csv")
    ap.add_argument("--frames", type=int, default=300,
                    help="frames per ROM; changing this rebuilds the two reusable ELFs")
    ap.add_argument("--jobs", type=int, default=max(1, min(4, os.cpu_count() or 1)))
    ap.add_argument("--timeout", type=int, default=300, help="seconds per QEMU run")
    ap.add_argument("--logs", type=Path, help="optional per-ROM raw logs")
    ap.add_argument("--rerun", action="store_true", help="ignore completed CSV rows")
    args = ap.parse_args()
    built_frames = RIG / "build_cost/frames.txt"
    current = built_frames.read_text().strip() if built_frames.exists() else ""
    elfs = [RIG / "build_cost/snes_cost_on.elf", RIG / "build_cost/snes_cost_off.elf"]
    deps = [RIG / "build_snes_cost.sh", RIG / "rig_snes.c", RIG / "rig_runtime_hf.c",
            RIG / "mps2_an500_snes.ld"]
    deps += list((ROOT / "external/sm/src/snes").glob("*.[ch]"))
    stale = (not all(p.exists() for p in elfs) or
             max(p.stat().st_mtime for p in deps) > min(p.stat().st_mtime for p in elfs))
    if current != str(args.frames) or stale:
        subprocess.run([str(RIG / "build_snes_cost.sh"), str(args.frames)],
                       cwd=ROOT, check=True)
    build_id = hashlib.sha256(b"".join(p.read_bytes() for p in elfs)).hexdigest()
    roms = roms_under(args.paths)
    if not roms:
        ap.error("no .smc/.sfc ROMs found")
    done: dict[str, tuple[str, str, str]] = {}
    if args.csv.exists() and not args.rerun:
        with args.csv.open(newline="", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            if reader.fieldnames != FIELDS:
                ap.error(f"{args.csv}: old/unknown CSV schema; pass --rerun to replace it")
            done = {row["rom"]: (row.get("rom_sha256", ""), row.get("build_id", ""),
                                 row.get("frames", ""))
                    for row in reader
                    if row.get("status") in {"PASS", "UNSUPPORTED"}}
    indexed = [(p, sha256(p)) for p in roms]
    pending = [(p, digest) for p, digest in indexed
               if done.get(os.path.relpath(p, ROOT)) != (digest, build_id, str(args.frames))]
    args.csv.parent.mkdir(parents=True, exist_ok=True)
    new_file = not args.csv.exists() or args.rerun
    mode = "w" if args.rerun else "a"
    complete = len(roms) - len(pending)
    print(f"SNES batch: {len(roms)} found, {complete} current, {len(pending)} pending")
    with args.csv.open(mode, newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS)
        if new_file: writer.writeheader()
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            futures = {pool.submit(profile, rom, digest, build_id, args.frames,
                                   args.timeout, args.logs): rom
                       for rom, digest in pending}
            for index, future in enumerate(as_completed(futures), 1):
                row = future.result(); writer.writerow(row); f.flush()
                print(f"[{index}/{len(pending)}] {row['status']:7} {row['rom']} "
                      f"ppu={row['ppu'] or '-'} dsp={row['dsp'] or '-'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
