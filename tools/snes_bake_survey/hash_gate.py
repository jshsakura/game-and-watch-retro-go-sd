#!/usr/bin/env python3
"""Prove SNES_SPIN_BAKE changes no emulation, across a whole ROM library.

The bake executes two of the cartridge's opcodes itself. Whether that is the
SAME machine is not a matter of argument -- it is the state and audio hashes,
per cartridge, with the feature off and on. This runs both arms of the M7 rig
(the engine the device runs, not a host interpreter, because the charges being
replayed are that engine's) over every ROM the recognizer installs into, and
reports any cartridge whose hashes move.

Resumable: results append to CSV and completed ROMs are skipped.

  hash_gate.py <rom-dir> [--frames 300] [--jobs 4] [--out gate.csv]
               [--only-installs survey.tsv]
"""
from __future__ import annotations
import argparse, csv, os, re, struct, subprocess, sys, tempfile
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed

ROOT = Path(__file__).resolve().parents[2]
RIG = ROOT / "tools/m7_qemu_rig"
FINAL = re.compile(r"STATEHASH=([0-9a-fA-F]+) AUDIOHASH=([0-9a-fA-F]+)")
BAKE = re.compile(r"\[bake\] on=(\d+) sites=(\d+) pc=(\S+) dp_off=(\S+) charge=\S+ laps=(\d+)")


def build(tag: str, extra: str, frames: int) -> Path:
    out = RIG / f"build_gate_{tag}"
    env = dict(os.environ, RIG_OUT=str(out), RIG_EXTRA_DEF=f"-DRIG_ROM_LOADER {extra}".strip())
    # The runner needs a ROM argument to embed; with RIG_ROM_LOADER the blob is
    # injected instead, so any small file will do for the link.
    subprocess.run(["bash", str(RIG / "run_snes_t2.sh"),
                    str(ROOT / "external/smw/smw.sfc"), str(frames)],
                   cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=1800)
    return out / "rig_snes.elf"


def run(elf: Path, rom: Path, timeout: int):
    size = rom.stat().st_size
    with tempfile.TemporaryDirectory(prefix="bake-gate-") as raw:
        tmp = Path(raw)
        link = tmp / "rom.sfc"
        link.symlink_to(rom)
        length = tmp / "len.bin"
        length.write_bytes(struct.pack("<I", size))
        cmd = ["qemu-system-arm", "-machine", "mps2-an500", "-nographic",
               "-semihosting", "-icount", "shift=0,align=off,sleep=off",
               "-kernel", str(elf),
               "-device", f"loader,file={link},addr=0x60800000,force-raw=on",
               "-device", f"loader,file={length},addr=0x607ffffc,force-raw=on"]
        p = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout)
    m = FINAL.search(p.stdout)
    if not m:
        raise RuntimeError((p.stdout.splitlines() or ["no output"])[-1][:120])
    b = BAKE.search(p.stdout)
    return m.group(1).lower(), m.group(2).lower(), (int(b.group(5)) if b else 0), \
           (b.group(3) if b else "-")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("romdir")
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--out", default="/tmp/snes_bake_gate.csv")
    ap.add_argument("--only-installs", help="survey.tsv: restrict to rows the scan installs")
    a = ap.parse_args()

    roms = sorted(p for p in Path(a.romdir).rglob("*")
                  if p.suffix.lower() in (".smc", ".sfc"))
    if a.only_installs:
        keep = {r.split("\t")[0] for r in Path(a.only_installs).read_text().splitlines()[1:]
                if r.split("\t")[1] == "OK"}
        roms = [p for p in roms if p.name in keep]

    done = set()
    out = Path(a.out)
    if out.exists():
        done = {r["name"] for r in csv.DictReader(out.open())}
    roms = [p for p in roms if p.name not in done]
    print(f"{len(roms)} ROMs to gate, {len(done)} already done, {a.jobs} jobs", file=sys.stderr)
    if not roms:
        return 0

    off = build("off", "", a.frames)
    on = build("on", "-DSNES_SPIN_BAKE", a.frames)

    fields = ["name", "verdict", "laps", "site", "state_off", "state_on",
              "audio_off", "audio_on", "note"]
    fh = out.open("a", newline="")
    w = csv.DictWriter(fh, fieldnames=fields)
    if not done:
        w.writeheader()

    def one(rom: Path):
        try:
            s0, a0, _, _ = run(off, rom, 900)
            s1, a1, laps, site = run(on, rom, 900)
        except Exception as e:                       # noqa: BLE001
            return {"name": rom.name, "verdict": "ERROR", "laps": 0, "site": "-",
                    "state_off": "", "state_on": "", "audio_off": "", "audio_on": "",
                    "note": str(e)[:120]}
        same = (s0 == s1 and a0 == a1)
        return {"name": rom.name,
                "verdict": "SAME" if same else "DIVERGED",
                "laps": laps, "site": site, "state_off": s0, "state_on": s1,
                "audio_off": a0, "audio_on": a1,
                "note": "" if same else "hashes differ"}

    bad = 0
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = {ex.submit(one, r): r for r in roms}
        for i, f in enumerate(as_completed(futs), 1):
            row = f.result()
            w.writerow(row); fh.flush()
            if row["verdict"] != "SAME":
                bad += 1
                print(f"  {row['verdict']}: {row['name']} {row['note']}", file=sys.stderr)
            if i % 25 == 0:
                print(f"  {i}/{len(roms)} ({bad} not SAME)", file=sys.stderr)
    fh.close()
    print(f"done: {bad} cartridges not SAME", file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
