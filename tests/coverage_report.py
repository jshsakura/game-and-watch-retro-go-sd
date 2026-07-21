#!/usr/bin/env python3
"""Reduce gcov --json-format output into the report tests/coverage.sh prints.

Input: a directory of *.gcov.json.gz (one per instrumented test binary, each
possibly covering multiple source files — see tests/coverage.sh for how those
get there) and tests/coverage_scope.txt (the MEASURED/UNMEASURED/EXCLUDED
list). Output: per-file and total line/branch coverage for MEASURED files,
plus the UNMEASURED work-order list, straight from the scope file so the
report and the scope file can never disagree about what "in scope" means.

A file compiled by more than one test binary (rg_clock.c: three configs;
rg_storage.c: two backends) is unioned here — a line counts as covered if ANY
run executed it — because the point is "does anything cover this line", not
any single run's number.
"""
import argparse
import gzip
import json
import os
import sys
from collections import defaultdict

REPO_ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))


def load_scope(path):
    measured, unmeasured, excluded = [], [], []
    with open(path, encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                print(f"coverage_scope.txt:{lineno}: expected 3 tab-separated "
                      f"fields, got {len(parts)} — skipping", file=sys.stderr)
                continue
            status, path_field, reason = parts[0], parts[1], parts[2]
            if status == "MEASURED":
                measured.append((path_field, reason))
            elif status == "UNMEASURED":
                unmeasured.append((path_field, reason))
            elif status == "EXCLUDED":
                excluded.append((path_field, reason))
            else:
                print(f"coverage_scope.txt:{lineno}: unknown status "
                      f"'{status}' — skipping", file=sys.stderr)
    return measured, unmeasured, excluded


def normalize(path):
    # gcov records #include "../Core/..." style paths as written relative to
    # the compiled TU (e.g. "tests/../Core/Src/retro-go/rg_alarm.c"); collapse
    # those to the repo-relative path the scope file uses as its key.
    return os.path.normpath(path)


def load_gcov_json(gcov_dir):
    """Returns {normalized_path: {line_no: covered_bool, ...}}, and a
    parallel branch map {normalized_path: [taken_bool, ...]} (unordered list —
    branch identity doesn't survive the union, only the taken/not-taken count
    does, which is what the summary needs)."""
    lines = defaultdict(dict)       # path -> {line_no: covered_bool}
    branches = defaultdict(list)    # path -> [bool, ...]
    n_files = 0
    for fn in sorted(os.listdir(gcov_dir)):
        if not fn.endswith(".gcov.json.gz"):
            continue
        n_files += 1
        with gzip.open(os.path.join(gcov_dir, fn)) as fh:
            data = json.load(fh)
        for entry in data.get("files", []):
            path = normalize(entry["file"])
            for line in entry.get("lines", []):
                ln = line["line_number"]
                covered = line.get("count", 0) > 0
                lines[path][ln] = lines[path].get(ln, False) or covered
                for br in line.get("branches", []):
                    branches[path].append(br.get("count", 0) > 0)
    return lines, branches, n_files


def pct(covered, total):
    return 100.0 * covered / total if total else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scope", required=True)
    ap.add_argument("--gcov-dir", required=True)
    args = ap.parse_args()

    measured, unmeasured, excluded = load_scope(args.scope)
    measured_paths = {normalize(p) for p, _ in measured}

    if not os.path.isdir(args.gcov_dir) or not os.listdir(args.gcov_dir):
        print("No gcov json data found — every build/run step must have "
              "been skipped. See the SKIP/FAIL lines above.", file=sys.stderr)
        line_data, branch_data, n = {}, {}, 0
    else:
        line_data, branch_data, n = load_gcov_json(args.gcov_dir)

    print(f"({n} instrumented .gcda file(s) reduced)")
    print()
    print("=" * 78)
    print("MEASURED — real gcov numbers from tests/run.sh's own test binaries")
    print("=" * 78)
    print(f"{'file':<52} {'lines':>11} {'branches':>12}")

    tot_lc = tot_lt = tot_bc = tot_bt = 0
    no_data = []
    rows = []
    for path, _reason in measured:
        npath = normalize(path)
        lmap = line_data.get(npath)
        if not lmap:
            no_data.append(path)
            continue
        lc = sum(1 for v in lmap.values() if v)
        lt = len(lmap)
        bmap = branch_data.get(npath, [])
        bc = sum(1 for v in bmap if v)
        bt = len(bmap)
        tot_lc += lc; tot_lt += lt; tot_bc += bc; tot_bt += bt
        rows.append((path, lc, lt, bc, bt))

    for path, lc, lt, bc, bt in sorted(rows, key=lambda r: pct(r[1], r[2])):
        lstr = f"{lc}/{lt} ({pct(lc, lt):5.1f}%)"
        bstr = f"{bc}/{bt} ({pct(bc, bt):5.1f}%)" if bt else "n/a"
        print(f"{path:<52} {lstr:>11} {bstr:>12}")

    print("-" * 78)
    tlstr = f"{tot_lc}/{tot_lt} ({pct(tot_lc, tot_lt):5.1f}%)"
    tbstr = f"{tot_bc}/{tot_bt} ({pct(tot_bc, tot_bt):5.1f}%)" if tot_bt else "n/a"
    print(f"{'TOTAL (MEASURED modules only)':<52} {tlstr:>11} {tbstr:>12}")

    if no_data:
        print()
        print("MEASURED in scope but NO coverage data collected this run "
              "(build/run was skipped or failed — see SKIP/FAIL lines above):")
        for path in no_data:
            print(f"  - {path}")

    print()
    print("=" * 78)
    print(f"UNMEASURED — in scope, no host harness exists yet ({len(unmeasured)} files)")
    print("This is the work order: risk-ranked in tests/coverage_scope.txt.")
    print("=" * 78)
    for path, reason in unmeasured:
        print(f"  {path}")
        print(f"      {reason}")

    print()
    print(f"EXCLUDED — {len(excluded)} entries (dirs or files); see "
          f"tests/coverage_scope.txt for each one's reason.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
