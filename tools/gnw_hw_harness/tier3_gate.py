#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def profile() -> int:
    raw = os.environ.get("GNW_DEVICE_PROFILE", "")
    if not raw:
        print("SKIP: GNW_DEVICE_PROFILE absent; absolute cycles/fps remain device-required")
        return 77
    path = Path(raw)
    if not path.is_file():
        print(f"FAIL GNW_DEVICE_PROFILE does not exist: {path}")
        return 1
    data = json.loads(path.read_text())
    clock = data.get("clock_hz") or data.get("clock", {}).get("hz")
    bindings = data.get("bindings", data.get("source", {}))
    if not isinstance(clock, int) or clock <= 0:
        print("FAIL device profile has no measured positive clock_hz")
        return 1
    if not any("map" in key and "sha" in key for key in bindings):
        print("FAIL device profile is not bound to a linker-map SHA")
        return 1
    print(f"PASS measured device profile: clk={clock}, map binding present")
    return 0


def checklist() -> int:
    data = json.loads((HERE / "tier3_checklist.json").read_text())
    main = (ROOT / "Core/Src/main.c").read_text(errors="replace")
    labelled = "SHCSR" in main and all(word in main for word in ("BUSFAULT", "USGFAULT", "MEMFAULT"))
    watchdog = "wdog_enable();" in main and "wdog_refresh();" in main
    print(f"PASS Tier3 checklist loaded: {len(data['device_required'])} device obligations")
    print(f"  host-observable fault labels: {'enabled' if labelled else 'disabled (device faults collapse to HardFault)'}")
    print(f"  watchdog wiring present: {'yes' if watchdog else 'no'}")
    print("SKIP: watchdog-window timing and fault-register truth require a device run")
    return 77


def delivery() -> int:
    print("SKIP: cycle-flow/IRQ/raster delivery equivalence cannot be proven by host state hashes")
    return 77


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("profile", "checklist", "delivery"))
    args = parser.parse_args()
    return {"profile": profile, "checklist": checklist, "delivery": delivery}[args.command]()


if __name__ == "__main__":
    raise SystemExit(main())
