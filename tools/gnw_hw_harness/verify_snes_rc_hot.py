#!/usr/bin/env python3
"""Post-build gate for the SNES SMW ITCM hot-site recompiler.

This script never builds firmware.  It consumes QEMU correctness logs and the
canonical device link artifacts produced by the release owner, then enforces:

* interpreter and hot-site STATEHASH/AUDIOHASH equality for SMW and Zelda;
* a non-empty .itcm_rc_hot that ends inside the device's 64 KiB ITCM;
* the SNES overlay (including its BSS) fits RAM_EMU;
* the 64 KiB PPU VRAM object is really in .overlay_snes_bss; and
* the linked image passes the live cross-overlay symbol-alias check.

It deliberately has no command that invokes make or make release.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import gnw_hw


ITCM_SECTION = ".itcm_rc_hot"
EXPECTED_VRAM_BYTES = 0x10000
HASH_RE = re.compile(
    r"STATEHASH=([0-9a-fA-F]+).*?AUDIOHASH=([0-9a-fA-F]+)"
)


class GateFailure(RuntimeError):
    pass


@dataclass(frozen=True)
class HashPair:
    state: str
    audio: str


def _require_file(path: Path, label: str) -> None:
    if not path.is_file():
        raise GateFailure(f"{label} not found: {path}")


def parse_hash_log(path: Path) -> HashPair:
    _require_file(path, "hash log")
    matches = HASH_RE.findall(path.read_text(encoding="utf-8", errors="replace"))
    if not matches:
        raise GateFailure(f"{path}: no line containing both STATEHASH and AUDIOHASH")
    state, audio = matches[-1]
    return HashPair(state.lower(), audio.lower())


def check_hashes(args: argparse.Namespace) -> None:
    cases = (
        ("SMW", args.baseline_smw_log, args.hot_smw_log),
        ("Zelda", args.baseline_zelda_log, args.hot_zelda_log),
    )
    for rom, baseline_path, hot_path in cases:
        baseline = parse_hash_log(baseline_path)
        hot = parse_hash_log(hot_path)
        if baseline != hot:
            raise GateFailure(
                f"{rom}: behavior changed: baseline STATEHASH={baseline.state} "
                f"AUDIOHASH={baseline.audio}, hot STATEHASH={hot.state} "
                f"AUDIOHASH={hot.audio}"
            )
        print(
            f"PASS correctness {rom}: STATEHASH={hot.state} "
            f"AUDIOHASH={hot.audio} bit-identical"
        )


def _tool(env_name: str, default: str) -> str:
    candidate = os.environ.get(env_name, default)
    resolved = shutil.which(candidate)
    if not resolved:
        raise GateFailure(f"required tool is unavailable: {candidate} ({env_name})")
    return resolved


def _run(command: list[str], *, env: dict[str, str] | None = None) -> str:
    result = subprocess.run(command, text=True, capture_output=True, env=env)
    if result.returncode:
        detail = (result.stdout + result.stderr).strip()
        raise GateFailure(f"command failed ({result.returncode}): {' '.join(command)}\n{detail}")
    return result.stdout


def _section_from_objdump(elf: Path, objdump: str) -> tuple[int, int]:
    output = _run([objdump, "-h", str(elf)])
    # Idx Name Size VMA LMA FileOff Algn
    pattern = re.compile(
        rf"^\s*\d+\s+{re.escape(ITCM_SECTION)}\s+"
        r"([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+",
        re.MULTILINE,
    )
    matches = pattern.findall(output)
    if len(matches) != 1:
        raise GateFailure(
            f"expected exactly one {ITCM_SECTION} in {elf}, found {len(matches)}"
        )
    size_hex, vma_hex = matches[0]
    return int(vma_hex, 16), int(size_hex, 16)


def _check_linker_assert(linker_script: Path) -> None:
    _require_file(linker_script, "linker script")
    text = linker_script.read_text(encoding="utf-8", errors="replace")
    section_at = text.find(ITCM_SECTION)
    if section_at < 0:
        raise GateFailure(f"{linker_script}: missing {ITCM_SECTION} output section")

    # Keep this structural rather than pinning symbol spellings.  The ASSERT
    # must be close to the section and name either the section or its rc-hot
    # boundary symbols, while constraining it against ITCM.
    nearby = text[section_at : section_at + 2200]
    # GNU ld expressions contain nested calls such as ABSOLUTE(), ORIGIN(),
    # and LENGTH(). A non-greedy parenthesis regex would stop at the first of
    # those, so inspect a bounded snippet beginning at each ASSERT instead.
    assertions = [
        nearby[match.start() : match.start() + 700]
        for match in re.finditer(r"\bASSERT\s*\(", nearby)
    ]
    if not any(
        ("ITCM" in assertion)
        and ("rc_hot" in assertion.lower() or "itcm_rc_hot" in assertion.lower())
        and ("<=" in assertion)
        for assertion in assertions
    ):
        raise GateFailure(
            f"{linker_script}: {ITCM_SECTION} has no nearby <= ITCM linker ASSERT"
        )


def _check_vram_symbol(elf: Path, objdump: str, ram_start: int, ram_end: int) -> None:
    output = _run([objdump, "-t", str(elf)])
    candidates = [line for line in output.splitlines() if "g_ppu_vram" in line]
    good: list[tuple[int, int, str]] = []
    for line in candidates:
        fields = line.split()
        if len(fields) < 6 or ".overlay_snes_bss" not in fields:
            continue
        try:
            address = int(fields[0], 16)
            section_index = fields.index(".overlay_snes_bss")
            size = int(fields[section_index + 1], 16)
        except (ValueError, IndexError):
            continue
        if size == EXPECTED_VRAM_BYTES:
            good.append((address, size, fields[-1]))

    if len(good) != 1:
        raise GateFailure(
            "expected one 65536-byte g_ppu_vram symbol in .overlay_snes_bss; "
            f"found {len(good)} (all name matches: {len(candidates)})"
        )
    address, size, name = good[0]
    if address < ram_start or address + size > ram_end:
        raise GateFailure(
            f"{name}: VRAM range 0x{address:x}..0x{address + size:x} is outside RAM_EMU"
        )
    print(
        f"PASS memory VRAM: {name}={size} bytes in .overlay_snes_bss "
        f"at 0x{address:08x}"
    )


def check_memory(args: argparse.Namespace) -> None:
    _require_file(args.map, "linker map")
    _require_file(args.elf, "ELF")
    _check_linker_assert(args.linker_script)

    contract = gnw_hw.extract_contract(args.map, args.elf)
    memory = contract["memory"]
    itcm_total = memory["itcm"]["total_bytes"]
    if itcm_total != 64 * 1024:
        raise GateFailure(f"device linker reports ITCM={itcm_total}, expected 65536")

    objdump = _tool("OBJDUMP", "arm-none-eabi-objdump")
    vma, size = _section_from_objdump(args.elf, objdump)
    if size <= 0:
        raise GateFailure(f"{ITCM_SECTION} is empty; hot code was not linked")
    if vma < 0 or vma + size > itcm_total:
        raise GateFailure(
            f"{ITCM_SECTION}: VMA 0x{vma:x} + {size} bytes exceeds "
            f"ITCM end 0x{itcm_total:x}"
        )
    print(
        f"PASS memory ITCM: {ITCM_SECTION}={size} bytes, "
        f"range 0x{vma:08x}..0x{vma + size:08x} <= 65536"
    )

    ram_emu = memory["ram_emu"]
    snes = ram_emu["overlays"].get("snes")
    if not snes:
        raise GateFailure("linker map has no _OVERLAY_SNES_SIZE/BSS_SIZE contract")
    if not snes["fits"]:
        raise GateFailure(
            f"SNES overlay uses {snes['total_bytes']} bytes, exceeds "
            f"RAM_EMU {ram_emu['total_bytes']} by {-snes['free_bytes']}"
        )
    print(
        f"PASS memory RAM_EMU: SNES image={snes['image_bytes']} "
        f"BSS={snes['bss_bytes']} total={snes['total_bytes']} "
        f"free={snes['free_bytes']}"
    )
    _check_vram_symbol(
        args.elf, objdump, ram_emu["start"], ram_emu["end"]
    )


def check_aliases(args: argparse.Namespace) -> None:
    if not args.build_dir.is_dir():
        raise GateFailure(f"build directory not found: {args.build_dir}")
    _require_file(args.elf, "ELF")
    nm = _tool("NM", "arm-none-eabi-nm")
    checker = args.repo / "scripts/check_core_symbol_aliases.py"
    _require_file(checker, "alias checker")
    env = os.environ.copy()
    env["NM"] = nm
    output = _run(
        [sys.executable, str(checker), str(args.build_dir), str(args.elf)], env=env
    )
    if "skipping" in output.lower() or "no nm" in output.lower():
        raise GateFailure("live alias checker skipped instead of inspecting the ELF")
    if not any(line.startswith("OK") for line in output.splitlines()):
        raise GateFailure(f"alias checker did not report an OK result:\n{output.strip()}")
    print("PASS aliasing: " + output.strip().splitlines()[-1])


def _add_hash_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--baseline-smw-log", type=Path, required=True)
    parser.add_argument("--hot-smw-log", type=Path, required=True)
    parser.add_argument("--baseline-zelda-log", type=Path, required=True)
    parser.add_argument("--hot-zelda-log", type=Path, required=True)


def _add_link_args(parser: argparse.ArgumentParser, repo: Path) -> None:
    parser.add_argument("--map", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument(
        "--linker-script",
        type=Path,
        default=repo / "STM32H7B0VBTx_SDCARD.ld",
    )


def make_parser(repo: Path) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    hashes = sub.add_parser("hashes", help="compare interpreter/hot QEMU hashes")
    _add_hash_args(hashes)

    memory = sub.add_parser("memory", help="gate ITCM/RAM_EMU/VRAM placement")
    memory.add_argument("--map", type=Path, required=True)
    memory.add_argument("--elf", type=Path, required=True)
    memory.add_argument(
        "--linker-script", type=Path, default=repo / "STM32H7B0VBTx_SDCARD.ld"
    )

    aliases = sub.add_parser("aliases", help="run live cross-overlay alias check")
    aliases.add_argument("--build-dir", type=Path, required=True)
    aliases.add_argument("--elf", type=Path, required=True)

    all_gate = sub.add_parser("all", help="run all three release-owner gates")
    _add_hash_args(all_gate)
    _add_link_args(all_gate, repo)
    return parser


def main(argv: list[str] | None = None) -> int:
    repo = Path(__file__).resolve().parents[2]
    args = make_parser(repo).parse_args(argv)
    args.repo = repo
    try:
        if args.command in {"hashes", "all"}:
            check_hashes(args)
        if args.command in {"memory", "all"}:
            check_memory(args)
        if args.command in {"aliases", "all"}:
            check_aliases(args)
    except (GateFailure, gnw_hw.ContractError, OSError) as exc:
        print(f"FAIL snes-rc-hot: {exc}", file=sys.stderr)
        return 1
    print("PASS snes-rc-hot: requested gates complete")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
