#!/usr/bin/env python3
"""One-shot host gate for the project's known device-only failure classes."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


@dataclass(frozen=True)
class Gate:
    gate_id: str
    title: str
    command: tuple[str, ...]
    tier: str = "host"
    timeout: int = 300


@dataclass
class Result:
    gate_id: str
    title: str
    tier: str
    status: str
    seconds: float
    detail: str


PY = sys.executable
SH = "bash"
GATES = (
    Gate("A01", "memory budgets + poison allocator", (SH, str(HERE / "run_tests.sh"))),
    Gate("B02-red", "64-bit alignment trap RED", (SH, str(HERE / "p0_regressions.sh"), "alignment")),
    Gate("B02-live", "actual SM alignment runtime", (SH, str(HERE / "existing_gate.sh"), "alignment-live")),
    Gate("B03", "implicit function declaration RED", (SH, str(HERE / "p0_regressions.sh"), "implicit-decl")),
    Gate("B04-red", "cross-core alias checker RED", (SH, str(HERE / "existing_gate.sh"), "alias-red")),
    Gate("B04-live", "linked ELF cross-core alias audit", (SH, str(HERE / "existing_gate.sh"), "alias-live")),
    Gate("B05", "poison malloc / zero calloc", (SH, str(HERE / "p0_regressions.sh"), "poison")),
    Gate("B06", "savestate stamp + refusal policy", (SH, str(HERE / "existing_gate.sh"), "savestate")),
    Gate("B07", "ITCM/XIP final-address integrity", (SH, str(HERE / "existing_gate.sh"), "xip-live")),
    Gate("B08", "caller wiring contracts", (SH, str(HERE / "existing_gate.sh"), "wiring")),
    Gate("B09", "device source list/defines parity", (SH, str(HERE / "p0_regressions.sh"), "parity"), timeout=600),
    Gate("B10", "over-aligned memcpy/memmove audit", (PY, str(HERE / "source_audit.py"), "overaligned-copy")),
    Gate("B11", "lang_t positional ABI", (PY, str(HERE / "source_audit.py"), "lang")),
    Gate("B12", "APPID /CONFIG version ABI", (PY, str(HERE / "source_audit.py"), "appid")),
    Gate("B13", "M7 hard-float ABI + CPACR", (PY, str(HERE / "source_audit.py"), "float-abi")),
    Gate("B14-red", "runner self-identity RED", (SH, str(HERE / "p0_regressions.sh"), "self-identical")),
    Gate("B14-live", "actual rig self-identity", (SH, str(HERE / "existing_gate.sh"), "repeat-live")),
    Gate("B-red", "static-policy RED fixtures", (PY, str(HERE / "test_source_audit.py"))),
    Gate("C15", "DWT/QEMU timing model", (PY, str(HERE / "test_timing_oracle.py")), tier="device-calibrated"),
    Gate("C15-live", "bound measured device profile", (PY, str(HERE / "tier3_gate.py"), "profile"), tier="device-required"),
    Gate("C16", "cycle-flow/IRQ delivery equivalence", (PY, str(HERE / "tier3_gate.py"), "delivery"), tier="device-required"),
    Gate("C17", "watchdog/fault device checklist", (PY, str(HERE / "tier3_gate.py"), "checklist"), tier="device-required"),
)


def run_gate(gate: Gate) -> Result:
    started = time.monotonic()
    try:
        proc = subprocess.run(
            gate.command,
            cwd=ROOT,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=gate.timeout,
        )
        if proc.returncode == 0:
            status = "PASS"
        elif proc.returncode == 77:
            status = "SKIP"
        else:
            status = "FAIL"
        detail = proc.stdout.strip()
    except subprocess.TimeoutExpired as exc:
        status = "FAIL"
        detail = f"timeout after {gate.timeout}s\n{exc.stdout or ''}"
    return Result(gate.gate_id, gate.title, gate.tier, status,
                  round(time.monotonic() - started, 3), detail)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", action="append", help="gate id/prefix (repeatable)")
    parser.add_argument("--json", type=Path, help="write machine-readable report")
    parser.add_argument("--require-device", action="store_true",
                        help="treat device-required SKIPs as failures (release sign-off)")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    selected = [g for g in GATES if not args.only or any(g.gate_id.startswith(x) for x in args.only)]
    if not selected:
        parser.error("--only selected no gates")
    results = []
    print("=== GNW SUPER GATE ===")
    for gate in selected:
        result = run_gate(gate)
        results.append(result)
        print(f"{result.status:4} {result.gate_id:8} {result.title} ({result.seconds:.2f}s)")
        if args.verbose or result.status == "FAIL":
            for line in result.detail.splitlines():
                print("     " + line)
    counts = {status: sum(r.status == status for r in results) for status in ("PASS", "FAIL", "SKIP")}
    print(f"=== SUMMARY PASS={counts['PASS']} FAIL={counts['FAIL']} SKIP={counts['SKIP']} ===")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({"results": [asdict(r) for r in results], "counts": counts}, indent=2) + "\n")
    failed = counts["FAIL"] > 0
    if args.require_device:
        failed = failed or any(r.status == "SKIP" and r.tier == "device-required" for r in results)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
