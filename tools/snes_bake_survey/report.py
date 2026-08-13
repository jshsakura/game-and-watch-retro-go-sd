#!/usr/bin/env python3
"""Turn the survey and the hash gate into one markdown report.

Mechanical on purpose: the inputs are thousands of rows and nobody should read
them to find the answer. It prints what a reviewer needs -- how much of the
library the recognizer touches, whether any cartridge's emulation moved, and how
often the loop actually runs -- and nothing else.

  report.py survey.tsv [gate.csv] > report.md
"""
from __future__ import annotations
import csv, sys
from collections import Counter
from pathlib import Path


def main() -> int:
    survey = Path(sys.argv[1])
    gate = Path(sys.argv[2]) if len(sys.argv) > 2 and Path(sys.argv[2]).exists() else None

    rows = [r for r in csv.DictReader(survey.open(encoding="utf-8"), delimiter="\t")]
    total = len(rows)
    status = Counter(r["status"] for r in rows)
    installs = [r for r in rows if r["status"] == "OK"]
    types = Counter(r["type"] for r in installs)
    sites = Counter(int(r["sites"]) for r in installs)

    p = print
    p("## Library survey\n")
    p(f"`tools/snes_bake_survey/run.sh` links the firmware's own `snes_loadRom()` and")
    p(f"`spin_bake_scan()`, so the mapper decision and the cart_read validation are the")
    p(f"ones that run on the device.\n")
    p(f"| | ROMs | |")
    p(f"|---|---:|---|")
    p(f"| scanned | {total} | |")
    for k, v in status.most_common():
        label = {"OK": "recognizer installs a loop", "NO_MATCH": "no match -- costs nothing",
                 "LOAD_FAIL": "cartridge not loadable by this core"}.get(k, k)
        p(f"| {k} | {v} | {label} ({100*v/total:.1f}%) |")
    p("")
    p(f"Of the {len(installs)} installs: "
      f"{types.get('1',0)} LoROM, {types.get('2',0)} HiROM. "
      f"Sites found per cartridge: " +
      ", ".join(f"{n}x{c}" for n, c in sorted(sites.items())) + ".\n")

    if not gate:
        p("_Hash gate not run yet._")
        return 0

    g = [r for r in csv.DictReader(gate.open())]
    verdicts = Counter(r["verdict"] for r in g)
    p("## Hash gate\n")
    p("Both arms of the M7 rig -- the Thumb-2 engine the device runs -- over every")
    p("cartridge the recognizer installs into, 300 frames each, comparing the state")
    p("and audio hashes with the feature off and on.\n")
    p(f"| verdict | cartridges |")
    p(f"|---|---:|")
    for k, v in verdicts.most_common():
        p(f"| {k} | {v} |")
    p("")
    diverged = [r for r in g if r["verdict"] == "DIVERGED"]
    if diverged:
        p("**Diverged:**\n")
        for r in diverged[:20]:
            p(f"- `{r['name']}` state {r['state_off']} -> {r['state_on']}, "
              f"audio {r['audio_off']} -> {r['audio_on']}")
        p("")
    errors = [r for r in g if r["verdict"] == "ERROR"]
    if errors:
        p(f"{len(errors)} cartridges could not be run by the rig at all "
          f"(unsupported mapper, boot hang, timeout); they are not evidence either way. "
          f"First few: " + ", ".join(f"`{r['name']}`" for r in errors[:5]) + "\n")

    fired = [r for r in g if r["verdict"] == "SAME" and int(r["laps"]) > 0]
    quiet = [r for r in g if r["verdict"] == "SAME" and int(r["laps"]) == 0]
    p("## How often the loop actually runs\n")
    p(f"`laps` counts replayed iterations in a 300-frame window from cold boot. That")
    p(f"window understates play: A Link to the Past replays 730 laps/frame in a boot")
    p(f"window and 3,976 in its play scene, a factor of 5.4. Read this as *whether* the")
    p(f"loop is live, not as the size of the prize.\n")
    p(f"- fired at least once: **{len(fired)}** of {len(fired)+len(quiet)} gated")
    p(f"- silent in the boot window: {len(quiet)}\n")
    if fired:
        top = sorted(fired, key=lambda r: -int(r["laps"]))[:15]
        p("| cartridge | laps / 300 frames | site |")
        p("|---|---:|---|")
        for r in top:
            p(f"| {r['name'][:52]} | {int(r['laps']):,} | `{r['site']}` |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
