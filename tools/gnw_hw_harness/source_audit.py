#!/usr/bin/env python3
"""Static policy gates for device-only failure classes.

These checks intentionally operate on git-tracked production sources. Untracked
work owned by another live session cannot make a commit gate fail, while a file
becomes in-scope as soon as it is staged/committed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BASELINE = HERE / "policy_baseline.json"


def digest(items: list[str]) -> str:
    return hashlib.sha256("\n".join(items).encode()).hexdigest()


def load_baseline() -> dict:
    return json.loads(BASELINE.read_text())


def tracked_sources() -> list[Path]:
    proc = subprocess.run(
        ["git", "ls-files", "-z", "--", "Core", "external", "retro-go-stm32"],
        cwd=ROOT,
        check=True,
        capture_output=True,
    )
    paths = []
    for raw in proc.stdout.split(b"\0"):
        if not raw:
            continue
        path = ROOT / raw.decode()
        if path.suffix.lower() in {".c", ".cc", ".cpp", ".h", ".hpp"} and path.is_file():
            paths.append(path)
    return paths


def audit_appid(_: argparse.Namespace) -> int:
    base = load_baseline()["appid"]
    header = (ROOT / "Core/Inc/retro-go/appid.h").read_text()
    enum = header[header.index("typedef enum"):header.index("APPID_COUNT")]
    apps = re.findall(r"^\s*(APPID_[A-Z0-9_]+)\s*(?:=\s*\d+)?\s*,", enum, re.M)
    if len(apps) < base["count"]:
        print(f"FAIL APPID_COUNT shrank: {len(apps)} < baseline {base['count']}")
        return 1
    prefix = apps[: base["count"]]
    if digest(prefix) != base["ordered_prefix_sha256"]:
        print("FAIL existing APPID order/value slots changed; /CONFIG app[] is positional")
        return 1
    settings = (ROOT / "Core/Src/porting/odroid_settings.c").read_text()
    match = re.search(r"\.version\s*=\s*(\d+)", settings)
    if not match:
        print("FAIL persistent_config_default.version not found")
        return 1
    version = int(match.group(1))
    added = len(apps) - base["count"]
    required = base["config_version"] + added
    if version < required:
        print(f"FAIL {added} APPID(s) appended but /CONFIG version is {version}; need >= {required}")
        return 1
    print(f"PASS APPID positional ABI: {len(apps)} slots, /CONFIG version {version}")
    return 0


def audit_lang(_: argparse.Namespace) -> int:
    base = load_baseline()["lang"]
    text = (ROOT / "Core/Inc/retro-go/rg_i18n_lang.h").read_text()
    body = text[text.index("typedef struct"):text.index("} lang_t;")]
    fields = re.findall(r"const\s+char\s*\*\s*(s_[A-Za-z0-9_]+)\s*;", body)
    if len(fields) < base["count"]:
        print(f"FAIL lang_t lost positional fields: {len(fields)} < baseline {base['count']}")
        return 1
    if digest(fields[: base["count"]]) != base["ordered_prefix_sha256"]:
        print("FAIL lang_t inserted/reordered/renamed a positional field; append-only ABI violated")
        return 1
    print(f"PASS lang_t positional ABI: {base['count']} pinned fields, {len(fields)-base['count']} appended")
    return 0


def _calls(text: str, name: str):
    """Yield simple balanced call bodies; enough for the direct-member hazard."""
    token = name + "("
    pos = 0
    while True:
        start = text.find(token, pos)
        if start < 0:
            return
        i = start + len(token)
        depth = 1
        while i < len(text) and depth:
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
            i += 1
        if depth == 0:
            yield start, text[start + len(token):i - 1]
        pos = max(i, start + len(token))


def audit_overaligned(args: argparse.Namespace) -> int:
    paths = [Path(args.path)] if args.path else tracked_sources()
    bad = []
    for path in paths:
        if not path.is_absolute():
            path = ROOT / path
        text = path.read_text(errors="replace")
        if "ABI_PTR_ALIGN" not in text and "aligned(8)" not in text and "aligned (8)" not in text:
            continue
        for func in ("memcpy", "memmove"):
            for offset, body in _calls(text, func):
                args0 = body.split(",", 2)[:2]
                if any("->" in item or re.search(r"\b\w+\.\w+", item) for item in args0):
                    line = text.count("\n", 0, offset) + 1
                    bad.append(f"{path.relative_to(ROOT) if path.is_relative_to(ROOT) else path}:{line}: {func}({body.strip()})")
    if bad:
        print("FAIL over-aligned member passed directly to memcpy/memmove; materialize plain locals")
        for item in bad:
            print("  " + item)
        return 1
    print(f"PASS over-aligned memcpy/memmove audit ({len(paths)} tracked sources)")
    return 0


def audit_float_abi(_: argparse.Namespace) -> int:
    base = load_baseline()
    exceptions = base["soft_float_exceptions"]
    scripts = sorted((ROOT / "tools/m7_qemu_rig").glob("run*.sh"))
    scripts += sorted((ROOT / "tools/m7_qemu_rig").glob("profile*.sh"))
    seen_exceptions = set()
    failures = []
    hard = 0
    for path in scripts:
        rel = str(path.relative_to(ROOT))
        text = path.read_text(errors="replace")
        if "-mcpu=cortex-m7" not in text:
            continue
        has_hard = "-mfloat-abi=hard" in text
        complete = has_hard and "-mfpu=fpv5-d16" in text and "-ffp-contract=off" in text
        if complete:
            hard += 1
            continue
        if rel in exceptions:
            seen_exceptions.add(rel)
            continue
        failures.append(rel)
    stale = set(exceptions) - seen_exceptions
    if failures or stale:
        for rel in failures:
            print(f"FAIL {rel}: M7 rig lacks hard-float/fpv5-d16/ffp-contract=off")
        for rel in sorted(stale):
            print(f"FAIL stale soft-float exception no longer matches a live M7 rig: {rel}")
        return 1
    runtime = (ROOT / "tools/m7_qemu_rig/rig_runtime.c").read_text(errors="replace")
    runtime_hf = (ROOT / "tools/m7_qemu_rig/rig_runtime_hf.c").read_text(errors="replace")
    if not all("0xE000ED88" in item and "0xFu << 20" in item
               for item in (runtime, runtime_hf)):
        print("FAIL hard-float rig runtime does not enable CP10/CP11 through CPACR")
        return 1
    print(f"PASS float ABI: {hard} hard-float M7 rigs; {len(exceptions)} explicit legacy exceptions")
    return 0


def savestate_candidate(path: Path, text: str) -> bool:
    return bool(re.search(r"\b(?:SaveState|save_state)\b", text) and
                re.search(r"\b(?:LoadState|load_state)\b", text) and
                ("fwrite(" in text or "fread(" in text))


def savestate_stamped(text: str) -> bool:
    has_magic = bool(re.search(r"\b(?:[A-Z0-9_]*MAGIC|magic)\b", text))
    has_version = bool(re.search(r"\b(?:[A-Z0-9_]*(?:VERSION|_VER)|version|ss_version)\b", text))
    has_length = bool(re.search(r"\b(?:payload_len|state_len|total_len|expected_len|length|file_size|ftell)\b", text))
    return has_magic and has_version and has_length


def audit_savestate(_: argparse.Namespace) -> int:
    exceptions = load_baseline()["unstamped_savestate_exceptions"]
    bad = []
    candidates = 0
    for path in tracked_sources():
        rel = str(path.relative_to(ROOT))
        if not rel.startswith("Core/Src/porting/"):
            continue
        text = path.read_text(errors="replace")
        if not savestate_candidate(path, text):
            continue
        candidates += 1
        if savestate_stamped(text):
            continue
        expected = exceptions.get(rel)
        actual = hashlib.sha256(text.encode()).hexdigest()
        if expected != actual:
            bad.append(rel)
    if bad:
        print("FAIL unstamped raw savestate implementation is new or changed")
        for rel in bad:
            print("  " + rel)
        print("Require magic + version + payload length, or pin reviewed legacy debt by exact SHA-256.")
        return 1
    print(f"PASS savestate stamp audit: {candidates} implementations, no new/changed unstamped raw dumps")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    for name, fn in (("appid", audit_appid), ("lang", audit_lang),
                     ("float-abi", audit_float_abi), ("savestate", audit_savestate)):
        p = sub.add_parser(name)
        p.set_defaults(fn=fn)
    p = sub.add_parser("overaligned-copy")
    p.add_argument("--path")
    p.set_defaults(fn=audit_overaligned)
    args = parser.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    raise SystemExit(main())
