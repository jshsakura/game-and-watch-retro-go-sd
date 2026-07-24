#!/usr/bin/env python3
"""Pack a CPS-1 MAME romset into a device-ready ``<Game Name>.cps1`` container.

For users who do NOT use the game-and-what web library: give it the MAME zip(s)
you already have and it writes the single flat file the Game & Watch firmware
reads. No web app, no account — just Python 3 (standard library only).

    python3 cps1_pack.py wofj.zip wof.zip -n "Warriors of Fate"
    # -> ./sd/roms/cps1/Warriors of Fate.cps1

Copy the resulting ``roms/`` (and ``covers/``, if you add art) onto the SD card
root. In the launcher the file lists and launches like any ROM.

WHAT IT PRODUCES (see docs/CPS1_LIBRARY_CONTRACT.md — this tool is one of its
three producers, alongside game-and-what and the firmware):

    roms/cps1/<Game Name>.cps1   = the romset's DISTINCT 512 KB chips
                                   concatenated back-to-back, uncompressed,
                                   no header, no index.

The device splits the file into 512 KB blocks and identifies each block by its
content CRC32 against tools/cps1_romsets.json (the shared romset table — this
tool READS it, never hard-codes a second copy). Block order is therefore
irrelevant, and the file name is just the display name (non-ASCII is fine).

A CPS-1 game is a multi-chip MAME romset; a clone archive (e.g. wofj.zip) holds
only the chips unique to it, so you usually pass the clone AND its parent
(wof.zip). If the inputs do not complete at least one known set, this refuses to
write a half-a-game file and tells you which parent set to add.
"""
from __future__ import annotations

import argparse
import json
import sys
import zipfile
import zlib
from pathlib import Path

CHIP_SIZE = 0x80000  # 512 KB — every CPS-1 chip; anything else is not a chip.
TABLE = Path(__file__).resolve().parent / "cps1_romsets.json"


def crc32(data: bytes) -> str:
    return "%08x" % (zlib.crc32(data) & 0xFFFFFFFF)


def load_romsets() -> list[dict]:
    table = json.loads(TABLE.read_text())
    if table.get("chip_size", CHIP_SIZE) != CHIP_SIZE:
        sys.exit(f"cps1_pack: {TABLE} chip_size != {CHIP_SIZE}")
    return table["romsets"]


def required_crcs(rs: dict) -> list[str]:
    """Every chip CRC a set needs, program chips first (device slot order)."""
    return [c["crc32"] for c in rs["prg"]] + [c["crc32"] for c in rs["gfx"]]


def collect_chips(inputs: list[Path]) -> "dict[str, bytes]":
    """CRC32 -> 512 KB bytes for every chip-sized member of every input.

    Inputs may be .zip archives, directories (scanned one level for zips and
    loose chip files), or loose chip files. First occurrence of a CRC wins; a
    second copy of identical bytes is the same chip, not a conflict. Members
    that are not exactly one chip long (PAL dumps, readmes) are ignored.
    """
    pool: dict[str, bytes] = {}

    def add_bytes(data: bytes) -> None:
        if len(data) == CHIP_SIZE:
            pool.setdefault(crc32(data), data)

    def add_path(p: Path) -> None:
        if p.is_dir():
            for child in sorted(p.iterdir()):
                if child.is_file():
                    add_path(child)
        elif zipfile.is_zipfile(p):
            with zipfile.ZipFile(p) as zf:
                for info in zf.infolist():
                    if not info.is_dir() and info.file_size == CHIP_SIZE:
                        add_bytes(zf.read(info))
        elif p.is_file() and p.stat().st_size == CHIP_SIZE:
            add_bytes(p.read_bytes())

    for inp in inputs:
        if not inp.exists():
            sys.exit(f"cps1_pack: no such input: {inp}")
        add_path(inp)
    return pool


def runnable_sets(pool: "dict[str, bytes]", romsets: list[dict]) -> list[dict]:
    have = set(pool)
    return [rs for rs in romsets if all(c in have for c in required_crcs(rs))]


def closest_set(pool: "dict[str, bytes]", romsets: list[dict]) -> "tuple[dict, list[str]]":
    have = set(pool)
    best, best_missing = None, None
    for rs in romsets:
        missing = [c for c in required_crcs(rs) if c not in have]
        if best_missing is None or len(missing) < len(best_missing):
            best, best_missing = rs, missing
    return best, best_missing


def build(inputs: list[Path], out_dir: Path, name: str | None) -> Path:
    romsets = load_romsets()
    pool = collect_chips(inputs)
    if not pool:
        sys.exit("cps1_pack: no 512 KB chips found in the inputs "
                 "(is this a CPS-1 romset zip?).")

    complete = runnable_sets(pool, romsets)
    if not complete:
        rs, missing = closest_set(pool, romsets)
        parent = rs.get("parent")
        hint = f" — add the parent set ({parent})" if parent else ""
        sys.exit(f"cps1_pack: incomplete romset. Closest is '{rs['name']}' "
                 f"({rs.get('title', '')}): {len(missing)} of "
                 f"{len(required_crcs(rs))} chips missing{hint}.\n"
                 f"           missing CRCs: {', '.join(missing)}")

    # Name the file for display: user override, else the first runnable set's
    # title, else its code. The device does not read the name — it is just what
    # the launcher shows — so anything filesystem-legal (incl. non-ASCII) works.
    if not name:
        rs = complete[0]
        name = rs.get("title") or rs["name"]
    name = "".join(c for c in name if c not in '\\/:*?"<>|').strip() or "cps1game"

    # The whole distinct pool, concatenated. Order is irrelevant to the device
    # (it identifies each block by content), so first-seen order is fine and
    # deterministic.
    blob = b"".join(pool.values())

    dst = out_dir / "roms" / "cps1" / f"{name}.cps1"
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(blob)

    print(f"cps1_pack: wrote {dst}")
    print(f"           {len(pool)} distinct chips, {len(blob)} bytes "
          f"({len(blob) // CHIP_SIZE} x 512 KB)")
    runnable_names = ", ".join(rs["name"] for rs in complete)
    print(f"           runnable set(s): {runnable_names}")
    print(f"Copy '{out_dir}/roms' (and any covers/) onto the SD card root.")
    return dst


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(
        description="Pack a CPS-1 MAME romset into a <Game Name>.cps1 container "
                    "for the Game & Watch firmware.",
        epilog="Pass a clone AND its parent (e.g. wofj.zip wof.zip) when the "
               "clone alone is incomplete.")
    ap.add_argument("inputs", nargs="+", type=Path,
                    help="MAME romset .zip file(s), a folder, or loose 512 KB chips")
    ap.add_argument("-o", "--out", type=Path, default=Path("sd"),
                    help="output root (default: ./sd); the file lands in "
                         "<out>/roms/cps1/")
    ap.add_argument("-n", "--name", default=None,
                    help="display name for the .cps1 (default: the romset title)")
    args = ap.parse_args(argv)
    build(args.inputs, args.out, args.name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
