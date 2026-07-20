#!/usr/bin/env python3
"""Game & Watch SD hardware-contract extractor and budget gate.

The linker map is authoritative for physical/static layout. Runtime facts that
cannot exist in a map (notably malloc use at emulator entry and the live clock)
come from a device profile bound to the exact map/ELF hashes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


SCHEMA_VERSION = 1
REQUIRED_SYMBOLS = (
    "__ITCMRAM_LENGTH__",
    "__RAM_UC_LENGTH__",
    "__RAM_EMU_LENGTH__",
    "__AHBRAM_LENGTH__",
    "__AHBRAM_AUDIO_RESERVE__",
    "__RAM_EMU_START__",
    "__RAM_EMU_END__",
    "__ahbram_heap_start__",
    "__ahbram_audio_start__",
    "_heap_start",
    "_heap_end",
)


class ContractError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_map(path: Path) -> tuple[dict[str, int], dict[str, dict[str, int]]]:
    symbols: dict[str, int] = {}
    regions: dict[str, dict[str, int]] = {}
    symbol_re = re.compile(
        r"^\s*(0x[0-9a-fA-F]+)\s+([A-Za-z_][A-Za-z0-9_.$]*)\s*(?:=|$)"
    )
    region_re = re.compile(
        r"^([A-Za-z_][A-Za-z0-9_]*)\s+"
        r"(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+[xrw]+\s*$"
    )
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            match = symbol_re.match(line)
            if match:
                # The definition appears before the cross-reference table. Keep
                # the first definition so a later textual mention cannot replace it.
                symbols.setdefault(match.group(2), int(match.group(1), 16))
            region = region_re.match(line.strip())
            if region:
                regions[region.group(1)] = {
                    "origin": int(region.group(2), 16),
                    "length": int(region.group(3), 16),
                }

    missing = [name for name in REQUIRED_SYMBOLS if name not in symbols]
    if missing:
        raise ContractError(f"map is missing required symbols: {', '.join(missing)}")
    if "AHBRAM" not in regions:
        raise ContractError("map is missing the AHBRAM memory-configuration row")
    return symbols, regions


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ContractError(f"cannot read JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ContractError(f"JSON root must be an object: {path}")
    return value


def _profile_runtime(profile: dict[str, Any] | None, key: str) -> int | None:
    if profile is None:
        return None
    value = profile.get("runtime", {}).get(key)
    if value is None:
        return None
    if not isinstance(value, int) or value < 0:
        raise ContractError(f"device profile runtime.{key} must be a non-negative integer")
    return value


def _overlay_budgets(symbols: dict[str, int], total: int) -> dict[str, Any]:
    overlays: dict[str, dict[str, int]] = {}
    for name, value in symbols.items():
        match = re.match(r"^_OVERLAY_(.+?)(_BSS)?_SIZE$", name)
        if not match:
            continue
        core = match.group(1).lower()
        field = "bss_bytes" if match.group(2) else "image_bytes"
        overlays.setdefault(core, {"image_bytes": 0, "bss_bytes": 0})[field] = value

    result: dict[str, Any] = {}
    for core, values in sorted(overlays.items()):
        used = values["image_bytes"] + values["bss_bytes"]
        result[core] = {
            **values,
            "total_bytes": used,
            "free_bytes": total - used,
            "fits": used <= total,
        }
    return result


def extract_contract(
    map_path: Path,
    elf_path: Path | None = None,
    profile_path: Path | None = None,
    config_path: Path | None = None,
) -> dict[str, Any]:
    symbols, regions = parse_map(map_path)
    map_hash = sha256_file(map_path)
    profile = _load_json(profile_path) if profile_path else None
    config = _load_json(config_path) if config_path else None
    config_hash = sha256_file(config_path) if config_path else None

    if profile is not None:
        if profile.get("schema_version") != SCHEMA_VERSION:
            raise ContractError("unsupported device-profile schema_version")
        bound_map = profile.get("build", {}).get("map_sha256")
        if not bound_map:
            raise ContractError("runtime device profile is not bound to a map_sha256")
        if bound_map != map_hash:
            raise ContractError(
                "stale device profile: map_sha256 does not match this linker map"
            )
        bound_elf = profile.get("build", {}).get("elf_sha256")
        if bound_elf:
            if elf_path is None:
                raise ContractError("profile is ELF-bound but --elf was not supplied")
            if bound_elf != sha256_file(elf_path):
                raise ContractError("stale device profile: elf_sha256 does not match")
        bound_config = profile.get("build", {}).get("config_sha256")
        if bound_config:
            if config_path is None:
                raise ContractError("profile is config-bound but --config was not supplied")
            if bound_config != config_hash:
                raise ContractError("stale device profile: config_sha256 does not match")

    dtcm_total = symbols["_heap_end"] - symbols["_heap_start"]
    # Upstream f25539a2 removed the fixed _Heap_Size constant: the DTCM heap now
    # simply fills whatever .data/.bss leave, so _heap_end-_heap_start IS the
    # size and there is no declared value left to cross-check it against. The
    # equivalent guard is that the heap must not grow into the stack reserve --
    # _heap_limit is what the linker script clamps it to.
    if dtcm_total <= 0:
        raise ContractError("DTCM heap is empty or inverted (_heap_end <= _heap_start)")
    heap_limit = symbols.get("_heap_limit")
    if heap_limit is not None and symbols["_heap_end"] > heap_limit:
        raise ContractError("DTCM heap runs past _heap_limit into the stack reserve")
    dtcm_used = _profile_runtime(profile, "dtcm_used_at_emu_init_bytes")
    if dtcm_used is not None and dtcm_used > dtcm_total:
        raise ContractError("device profile reports DTCM use beyond the heap")

    ahb_origin = regions["AHBRAM"]["origin"]
    ahb_physical = regions["AHBRAM"]["length"]
    if ahb_physical != symbols["__AHBRAM_LENGTH__"]:
        raise ContractError("AHBRAM region length disagrees with linker symbol")
    ahb_audio_start = symbols["__ahbram_audio_start__"]
    ahb_heap_start = symbols["__ahbram_heap_start__"]
    ahb_before_audio = ahb_audio_start - ahb_origin
    ahb_static = ahb_heap_start - ahb_origin
    ahb_dynamic = ahb_audio_start - ahb_heap_start
    if min(ahb_before_audio, ahb_static, ahb_dynamic) < 0:
        raise ContractError("AHB linker symbols are out of order")
    if ahb_physical - ahb_before_audio != symbols["__AHBRAM_AUDIO_RESERVE__"]:
        raise ContractError("AHB audio reserve disagrees with heap/audio symbols")

    ram_emu_total = symbols["__RAM_EMU_END__"] - symbols["__RAM_EMU_START__"]
    if ram_emu_total != symbols["__RAM_EMU_LENGTH__"]:
        raise ContractError("RAM_EMU start/end disagree with __RAM_EMU_LENGTH__")

    clock_profiles = profile.get("clock_profiles", []) if profile else []
    contract: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "authority": {
            "static_layout": "linker-map",
            "runtime_memory": "device-profile" if profile else "unavailable",
            "clock": "device-profile" if clock_profiles else "unavailable",
            "qemu_timing_claim": "instruction-count-only",
        },
        "build": {
            "map": str(map_path),
            "map_sha256": map_hash,
            "elf": str(elf_path) if elf_path else None,
            "elf_sha256": sha256_file(elf_path) if elf_path else None,
            "device_profile": str(profile_path) if profile_path else None,
            "config_manifest": str(config_path) if config_path else None,
            "config_sha256": config_hash,
            "config": config,
        },
        "memory": {
            "itcm": {"total_bytes": symbols["__ITCMRAM_LENGTH__"]},
            "dtcm_heap": {
                "start": symbols["_heap_start"],
                "end": symbols["_heap_end"],
                "total_bytes": dtcm_total,
                "used_at_emu_init_bytes": dtcm_used,
                "effective_free_at_emu_init_bytes": (
                    dtcm_total - dtcm_used if dtcm_used is not None else None
                ),
            },
            "framebuffer_axi": {"reserved_bytes": symbols["__RAM_UC_LENGTH__"]},
            "ram_emu": {
                "start": symbols["__RAM_EMU_START__"],
                "end": symbols["__RAM_EMU_END__"],
                "total_bytes": ram_emu_total,
                "overlays": _overlay_budgets(symbols, ram_emu_total),
            },
            "ahb": {
                "origin": ahb_origin,
                "physical_total_bytes": ahb_physical,
                "audio_reserved_bytes": symbols["__AHBRAM_AUDIO_RESERVE__"],
                "allocatable_before_audio_bytes": ahb_before_audio,
                "static_reserved_before_heap_bytes": ahb_static,
                "heap_start": ahb_heap_start,
                "effective_dynamic_free_bytes": ahb_dynamic,
            },
        },
        "clock_profiles": clock_profiles,
        "device": profile.get("device", {}) if profile else {},
    }
    return contract


def dotted_get(value: dict[str, Any], path: str) -> Any:
    current: Any = value
    for part in path.split("."):
        if isinstance(current, dict) and part in current:
            current = current[part]
        elif isinstance(current, list) and part.isdigit() and int(part) < len(current):
            current = current[int(part)]
        else:
            raise ContractError(f"contract has no field {path}")
    return current


def verify_expected(contract: dict[str, Any], expected_path: Path) -> list[str]:
    document = _load_json(expected_path)
    expected = document.get("expected")
    if not isinstance(expected, dict):
        raise ContractError("golden JSON must contain an expected object")
    failures: list[str] = []
    for path, wanted in expected.items():
        try:
            actual = dotted_get(contract, path)
        except ContractError as exc:
            failures.append(str(exc))
            continue
        if actual != wanted:
            failures.append(f"{path}: expected {wanted!r}, got {actual!r}")
    return failures


def allocation_free(contract: dict[str, Any], region: str) -> int | None:
    paths = {
        "dtcm": "memory.dtcm_heap.effective_free_at_emu_init_bytes",
        "ahb": "memory.ahb.effective_dynamic_free_bytes",
        "itcm": "memory.itcm.total_bytes",
        "ram_emu": "memory.ram_emu.total_bytes",
    }
    if region not in paths:
        raise ContractError(f"unknown region {region}")
    value = dotted_get(contract, paths[region])
    if value is not None and not isinstance(value, int):
        raise ContractError(f"non-integer free space for {region}")
    return value


def render_report(contract: dict[str, Any]) -> str:
    memory = contract["memory"]
    dtcm = memory["dtcm_heap"]
    ahb = memory["ahb"]
    clocks = contract.get("clock_profiles", [])
    clock_text = ", ".join(
        f"{item.get('name', '?')}={item.get('core_hz', '?')}Hz" for item in clocks
    ) or "UNAVAILABLE (device measurement required)"
    dtcm_free = dtcm["effective_free_at_emu_init_bytes"]
    dtcm_text = str(dtcm_free) if dtcm_free is not None else "UNKNOWN (profile required)"
    return "\n".join(
        (
            "GNW SD hardware contract",
            f"  map sha256: {contract['build']['map_sha256']}",
            f"  clocks: {clock_text}",
            f"  ITCM: {memory['itcm']['total_bytes']} bytes",
            f"  DTCM heap: {dtcm['total_bytes']} bytes; effective free at emu init: {dtcm_text}",
            f"  framebuffer AXI reserve: {memory['framebuffer_axi']['reserved_bytes']} bytes",
            f"  RAM_EMU: {memory['ram_emu']['total_bytes']} bytes",
            "  AHB: "
            f"{ahb['physical_total_bytes']} physical; {ahb['audio_reserved_bytes']} audio; "
            f"{ahb['static_reserved_before_heap_bytes']} static; "
            f"{ahb['effective_dynamic_free_bytes']} dynamic free",
            "  QEMU timing authority: instruction counts only; absolute cycles require device profile",
        )
    )


def profile_from_log(args: argparse.Namespace) -> dict[str, Any]:
    log_text = args.log.read_text(encoding="utf-8", errors="replace")
    clocks = [int(value) for value in re.findall(r"\bclk=(\d+)\b", log_text)]
    oom = re.findall(r"HEAP OOM:\s*need=(\d+)\s+used=(\d+)/(\d+)", log_text)
    used = int(oom[-1][1]) if oom else None
    total = int(oom[-1][2]) if oom else None
    if args.dtcm_used is not None:
        used = args.dtcm_used
    if not clocks and args.clock_hz is None:
        raise ContractError("log has no clk= field; pass --clock-hz")
    core_hz = args.clock_hz if args.clock_hz is not None else clocks[-1]
    if used is None:
        raise ContractError("log has no HEAP OOM used field; pass --dtcm-used")
    symbols, _ = parse_map(args.map)
    map_total = symbols["_heap_end"] - symbols["_heap_start"]
    if total is not None and total != map_total:
        raise ContractError("HEAP OOM total does not match the supplied map")
    return {
        "schema_version": SCHEMA_VERSION,
        "device": {"target": args.target, "sd_hardware": args.sd_hardware},
        "build": {
            "map_sha256": sha256_file(args.map),
            "elf_sha256": sha256_file(args.elf) if args.elf else None,
            "config_sha256": (
                sha256_file(args.config_manifest) if args.config_manifest else None
            ),
            "config": args.config,
        },
        "runtime": {"dtcm_used_at_emu_init_bytes": used},
        "clock_profiles": [
            {"name": args.profile_name, "core_hz": core_hz, "source": "device-log"}
        ],
    }


def write_json(value: dict[str, Any], output: Path | None) -> None:
    text = json.dumps(value, indent=2, sort_keys=True) + "\n"
    if output:
        output.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    extract = sub.add_parser("extract", help="extract a contract from a GNU ld map")
    extract.add_argument("--map", type=Path, required=True)
    extract.add_argument("--elf", type=Path)
    extract.add_argument("--profile", type=Path)
    extract.add_argument("--config", type=Path)
    extract.add_argument("--golden", type=Path)
    extract.add_argument("--output", type=Path)
    extract.add_argument("--report", action="store_true")

    check = sub.add_parser("check-allocation", help="gate one proposed allocation")
    check.add_argument("--spec", type=Path, required=True)
    check.add_argument("--region", choices=("dtcm", "ahb", "itcm", "ram_emu"), required=True)
    check.add_argument("--bytes", type=int, required=True)
    check.add_argument("--label", default="allocation")

    report = sub.add_parser("report", help="render a saved contract")
    report.add_argument("--spec", type=Path, required=True)

    profile = sub.add_parser("profile-from-log", help="bind device log facts to a map/ELF")
    profile.add_argument("--log", type=Path, required=True)
    profile.add_argument("--map", type=Path, required=True)
    profile.add_argument("--elf", type=Path)
    profile.add_argument("--output", type=Path)
    profile.add_argument("--profile-name", default="profile2")
    profile.add_argument("--clock-hz", type=int)
    profile.add_argument("--dtcm-used", type=int)
    profile.add_argument("--target", choices=("mario", "zelda"), default="mario")
    profile.add_argument("--sd-hardware", default="unknown")
    profile.add_argument("--config", default="canonical-sd")
    profile.add_argument("--config-manifest", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "extract":
            contract = extract_contract(args.map, args.elf, args.profile, args.config)
            if args.golden:
                failures = verify_expected(contract, args.golden)
                if failures:
                    for failure in failures:
                        print(f"FAIL golden: {failure}", file=sys.stderr)
                    return 1
            if args.report:
                print(render_report(contract))
            write_json(contract, args.output)
            return 0
        if args.command == "check-allocation":
            contract = _load_json(args.spec)
            free = allocation_free(contract, args.region)
            if free is None:
                raise ContractError(
                    f"{args.region} effective free space is unknown; attach a bound device profile"
                )
            requested = (args.bytes + 3) & ~3
            if requested > free:
                print(
                    f"FAIL {args.label}: {requested} bytes exceeds {args.region} effective free {free}",
                    file=sys.stderr,
                )
                return 1
            print(f"PASS {args.label}: {requested} <= {free} bytes in {args.region}")
            return 0
        if args.command == "report":
            print(render_report(_load_json(args.spec)))
            return 0
        if args.command == "profile-from-log":
            write_json(profile_from_log(args), args.output)
            return 0
    except (ContractError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    raise AssertionError("unreachable")


if __name__ == "__main__":
    raise SystemExit(main())
