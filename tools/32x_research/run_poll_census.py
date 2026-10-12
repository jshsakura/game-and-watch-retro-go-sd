#!/usr/bin/env python3
"""One finite, isolated census of existing polling-helper host iterations.

No hardware access, no retry, no status polling, no ROM copy. Completed baseline
objects, ROM objects and results are reused. Diagnostic timing is discarded.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

import run_aot_blocks as rig

ROOT = rig.ROOT
PRIOR = ROOT / "build/32x-fixed-thumb-v4-20261007"
HEADER = ROOT / "tools/32x_research/poll_census.h"
KINDS = ["word-test-BT/BF", "long-compare-BT/S", "GBR-word-BFS"]
FIELDS = ["calls", "active_calls", "iterations", "removable_reads",
          "value_changes", "indirect_calls", "max_iterations"]


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def instrument(source):
    """Add counters to exactly the three audited loops; retain original reads."""
    anchor = "static void gnw_sh2_fastloop(SH2 *sh2, UINT32 opcode)\n{"
    assert source.count(anchor) == 1
    source = source.replace(anchor, '#include "poll_census.h"\n\n' + anchor)
    patterns = [
        (0, "pa", "unsigned int val = (UINT32)(UINT16)RW(sh2, pa);",
         "t", "want_t_loop"),
        (1, "pa", "sh2->r[dest] = (UINT32)RL(sh2, pa);", None, None),
        (2, "poll_addr", "unsigned int val = (unsigned int)(UINT16)RW(sh2, poll_addr);",
         None, None),
    ]
    needle = "while (sh2->icount >= 5) {"
    assert source.count(needle) == 3
    # Replace the three regions backwards so earlier indices stay valid.
    regions = []
    for match in re.finditer(re.escape(needle), source):
        start = match.start()
        brace = source.index("{", start)
        depth = 1
        end = brace + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        regions.append((start, end))
    for (start, end), (kind, address, read, _, _) in reversed(list(zip(regions, patterns))):
        block = source[start:end]
        assert block.count(read) == 1
        value = "sh2->r[dest]" if kind == 1 else "val"
        block = block.replace(read, read + f"\n poll_census_read(&poll_sample, {value});")
        # Both braced early exits and BT/S's bare if-return must finish counts.
        if kind == 1:
            assert "if (!t)\n\t\t\t\t\t\treturn;" in block
            block = block.replace("if (!t)\n\t\t\t\t\t\treturn;",
                "if (!t) { poll_census_end(&poll_sample, 1); return; }")
        else:
            assert block.count("return;") == 1
            block = block.replace("return;", f"poll_census_end(&poll_sample, {kind}); return;")
        block = (f"struct poll_census_sample poll_sample = poll_census_begin(sh2, {kind}, {address});\n" +
                 block + f"\n poll_census_end(&poll_sample, {kind});")
        source = source[:start] + block + source[end:]
    return source


def compile_arm(args, out, title):
    prior = PRIOR / f"{title}-baseline-clean"
    meta = json.loads((prior / "inputs.json").read_text())
    assert rig.digest(prior / "rig_32x.elf") == meta["elf_sha256"]
    assert rig.digest(Path(meta["rom"])) == meta["rom_sha256"]
    case = out / title
    case.mkdir()
    for obj in prior.glob("*.o"):
        shutil.copyfile(obj, case / obj.name)
    defs = meta["defs"]
    with (case / "build.log").open("w") as log:
        def cc(argv):
            if args.compiler == "docker":
                rig.cc(argv, log)
            else:
                subprocess.run(["arm-none-eabi-gcc", *rig.ARCH, *rig.FLAGS, *argv],
                    cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                    check=True, timeout=180)
        cc([*defs, "-I" + rig.rel(out / "source"), "-I" + rig.rel(out),
            "-c", rig.rel(out / "source/cpu/sh2/mame/sh2pico.c"),
            "-o", rig.rel(case / "cpu_sh2_mame_sh2pico.o")])
        cc([*defs, "-I" + rig.rel(out / "source"), "-I" + rig.rel(out / "source/pico"),
            "-I" + rig.rel(PRIOR / "rig"), "-c", rig.rel(out / "rig_32x.c"),
            "-o", rig.rel(case / "rig_32x.o")])
        cc(["-T" + rig.rel(PRIOR / "rig/mps2_an500_32x.ld"), "-nostartfiles",
            "-Wl,--gc-sections", "-Wl,-Map=" + rig.rel(case / "rig.map"),
            *[rig.rel(p) for p in sorted(case.glob("*.o"))], "-lm", "-lc",
            "-o", rig.rel(case / "rig_32x.elf")])
    meta.update(diagnostic=True, guest_observer=False, compiler_mode=args.compiler,
                prior_arm=str(prior), elf_sha256=rig.digest(case / "rig_32x.elf"),
                modified_source_sha256=rig.digest(out / "source/cpu/sh2/mame/sh2pico.c"),
                census_header_sha256=rig.digest(out / "poll_census.h"),
                diagnostic_timing_usable=False)
    save(case / "inputs.json", meta)
    return case


def run_arm(case, timeout):
    # One blocking subprocess wait is the completion mechanism.
    with (case / "qemu.log").open("w") as log:
        subprocess.run(["qemu-system-arm", "-machine", "mps2-an500", "-nographic",
            "-semihosting", "-kernel", str(case / "rig_32x.elf")], cwd=ROOT,
            stdout=log, stderr=subprocess.STDOUT, check=True, timeout=timeout)
    text = (case / "qemu.log").read_text()
    meta = json.loads((case / "inputs.json").read_text())
    prior = Path(meta["prior_arm"])
    old = (prior / "qemu.log").read_text()
    signatures = lambda s: [line for line in s.splitlines() if any(
        key in line for key in ("snd hash=", "snd energy", "fb f"))]
    snapshots = lambda s: re.findall(r"\[fbd\] frame=.*?\[fbd\] end", s, re.S)
    assert "GATE3 PASS" in text and "POLL_CENSUS_END" in text
    assert f"done {meta['window'][1]} frames" in text
    assert signatures(text) == signatures(old)
    assert len(snapshots(text)) == 2 and snapshots(text) == snapshots(old)
    rows = []
    for values in re.findall(r"^POLL_CENSUS (.+)$", text, re.M):
        core, kind, *counts = map(int, values.split())
        assert len(counts) == len(FIELDS)
        rows.append(dict(core=core, kind=KINDS[kind], **dict(zip(FIELDS, counts))))
    assert len(rows) == 6
    result = dict(status="completed_equal_diagnostic", window=meta["window"],
        device_access=False, diagnostic_timing_discarded=True,
        framebuffer_endpoints_equal=True, audio_signatures_equal=True,
        guest_dispatches_measured=False, loops=rows,
        total_iterations=sum(r["iterations"] for r in rows),
        removable_reads=sum(r["removable_reads"] for r in rows),
        value_changes=sum(r["value_changes"] for r in rows),
        performance_gain_measured=False)
    save(case / "result.json", result)
    print(json.dumps({"case": case.name, **result}), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", choices=["docker", "local"], default="docker")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    out.relative_to(ROOT)
    out.mkdir(parents=True, exist_ok=False)
    state = dict(status="preparing", hypothesis="S01", device_access=False,
                 device_state="untouched", current_best="PCM-r2 retained")
    save(out / "state.json", state)
    try:
        shutil.copytree(PRIOR / "baseline", out / "source")
        source = out / "source/cpu/sh2/mame/sh2pico.c"
        original_hash = rig.digest(source)
        assert original_hash == "ce3921dbe4a8fb8814d7915001ff1af48cce9b9a4f54d029001fe91fb50e1e47"
        source.write_text(instrument(source.read_text()))
        shutil.copyfile(HEADER, out / "poll_census.h")
        harness = (PRIOR / "rig/rig_32x_clean.c").read_text()
        marker = "    return pass ? 0 : 4;"
        position = harness.rfind(marker)
        assert position > 0
        harness = harness[:position] + "    extern void poll_census_report(void);\n    poll_census_report();\n" + harness[position:]
        (out / "rig_32x.c").write_text(harness)
        state.update(status="prepared", source_sha256=original_hash,
                     modified_source_sha256=rig.digest(source), compiler_mode=args.compiler,
                     runner_sha256=rig.digest(Path(__file__)), header_sha256=rig.digest(HEADER),
                     cases=["after-burner", "doom"], diagnostic_timing_usable=False)
        save(out / "state.json", state)
        if args.prepare_only:
            print(json.dumps(state), flush=True)
            return
        results = {}
        for title in state["cases"]:
            case = compile_arm(args, out, title)
            print("BUILT " + title, flush=True)
            results[title] = run_arm(case, args.timeout)
        save(out / "result.json", dict(status="completed_census", cases=results,
             device_access=False, physical_fps_measured=False,
             limitation="Counts are diagnostic opportunities, not measured savings or proof of equivalence of a replacement fold."))
        state.update(status="completed_census", last_result=str(out / "result.json"),
                     next="Review actual iteration counts before any candidate or hardware work")
        save(out / "state.json", state)
    except Exception as error:
        state.update(status="failed", error_type=type(error).__name__, error=str(error),
                     automatic_retry=False)
        save(out / "state.json", state)
        raise


if __name__ == "__main__":
    main()
