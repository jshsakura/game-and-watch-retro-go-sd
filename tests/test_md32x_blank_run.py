#!/usr/bin/env python3
"""Compare every output byte of the original and optimized MD tile strips."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def compare(source):
    with tempfile.TemporaryDirectory(prefix="md32x-blank-run-") as directory:
        binaries = []
        for name, defines in (("baseline", ["-DGNW_MD_NO_BLANK_RUN"]),
                              ("candidate", [])):
            binary = Path(directory) / name
            subprocess.run([
                os.environ.get("CC", "cc"), "-O2", "-fno-strict-aliasing",
                "-ffunction-sections", "-fdata-sections", "-DGNW_32X_CORE",
                "-DEMU_G68K", "-DTABLES_FULL", "-D_USE_CZ80", *defines,
                "-Iexternal/picodrive", source,
                "-Wl,--gc-sections", "-o", str(binary),
            ], cwd=ROOT, check=True)
            binaries.append(binary)

        processes = [subprocess.Popen([str(binary)], stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE) for binary in binaries]
        offset = 0
        try:
            while True:
                a, b = (p.stdout.read(65536) for p in processes)
                if a != b:
                    first = next((i for i, (x, y) in enumerate(zip(a, b))
                                  if x != y), min(len(a), len(b)))
                    raise RuntimeError(f"render mismatch at output byte {offset + first}")
                offset += len(a)
                if not a:
                    break
            for p in processes:
                if p.wait() != 0:
                    raise RuntimeError(p.stderr.read().decode())
                print(p.stderr.read().decode().strip())
            print(f"{source}: PASS, all {offset} output bytes match")
        finally:
            for p in processes:
                if p.poll() is None:
                    p.terminate()
                p.wait()


if __name__ == "__main__":
    compare("tests/test_md32x_blank_run.c")
