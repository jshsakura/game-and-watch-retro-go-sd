#!/usr/bin/env python3
"""Run one command repeatedly and require byte-identical observable output."""

from __future__ import annotations

import argparse
import hashlib
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command or args.runs < 2:
        parser.error("provide a command after -- and at least two runs")
    outputs = []
    for run in range(args.runs):
        proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if proc.returncode:
            print(f"FAIL repeatability run {run + 1} exited {proc.returncode}")
            print(proc.stdout.decode(errors="replace"))
            return 1
        outputs.append(proc.stdout)
    hashes = [hashlib.sha256(item).hexdigest() for item in outputs]
    if len(set(hashes)) != 1:
        print("FAIL same binary disagrees with itself: " + ", ".join(h[:12] for h in hashes))
        return 1
    print(f"PASS {args.runs} identical runs: {hashes[0][:16]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
