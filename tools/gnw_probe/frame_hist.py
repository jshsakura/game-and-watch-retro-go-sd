#!/usr/bin/env python3
"""Read the SNES per-frame work histogram out of a running device over SWD.

    tools/gnw_probe/frame_hist.py <elf-built-with-SNES_FRAME_HIST=1>

Why this exists: the paced frame counter measures audio periods, not work. A
frame that finishes in half a period and one that finishes in nine tenths score
the same; one that takes 1.01 periods costs two. So "57.3 fps" is a statement
about how many frames cross the period line, and until now nothing in the build
could say which frames those were or by how much they missed. This prints the
distribution and marks the line.
"""
import re
import subprocess
import sys

HOST_DEFAULT = "rpi-genie5"
OC = ("sudo openocd -f interface/stlink-dap.cfg -f target/stm32h7x.cfg "
      "-c 'adapter speed 4000'")
SHIFT = 18
BUCKETS = 48


def sym(elf, name):
    out = subprocess.run(["arm-none-eabi-nm", elf], capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] in (name, "gsnes__" + name):
            return int(p[0], 16)
    return None


def read_words(host, addr, n):
    cmd = f"{OC} -c init -c 'mdw 0x{addr:08x} {n}' -c shutdown 2>&1"
    out = subprocess.run(["ssh", host, cmd], capture_output=True, text=True).stdout
    words = []
    for line in out.splitlines():
        m = re.match(r"^0x[0-9a-f]+:((?:\s[0-9a-f]{8})+)", line)
        if m:
            words += [int(w, 16) for w in m.group(1).split()]
    return words[:n]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    elf, host = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else HOST_DEFAULT)

    base = sym(elf, "snes_fh_bucket")
    n_addr = sym(elf, "snes_fh_n")
    max_addr = sym(elf, "snes_fh_max")
    if base is None or n_addr is None:
        sys.exit("no snes_fh_* symbols: build with SNES_FRAME_HIST=1")

    buckets = read_words(host, base, BUCKETS)
    n = read_words(host, n_addr, 1)[0]
    mx = read_words(host, max_addr, 1)[0]
    total = sum(buckets)
    if not total:
        sys.exit("histogram is empty: is a SNES ROM running?")

    # Core clock and the audio period the pacing waits on. Both are build
    # constants on this device; printed so a wrong one is visible, not silent.
    clk, period_hz = 340e6, 60.15
    per_bucket_ms = (1 << SHIFT) / clk * 1e3
    line = (clk / period_hz) / (1 << SHIFT)

    print(f"frames={n}  max={mx} cyc ({mx / clk * 1e3:.2f} ms)  "
          f"bucket={per_bucket_ms:.2f} ms  period line = bucket {line:.1f}")
    over = sum(c for i, c in enumerate(buckets) if i > line)
    print(f"over one audio period: {over}/{total} = {100 * over / total:.1f}%  "
          f"-> paced fps ceiling ~= {period_hz / (1 + over / total):.2f}")
    # Where the slow frames spend the extra time. Two 64-bit sums each, little
    # endian, so four words per pair.
    def u64(name):
        a = sym(elf, name)
        if a is None:
            return None
        lo, hi = read_words(host, a, 2)
        return lo | (hi << 32)

    n_over = read_words(host, sym(elf, "snes_fh_n_over"), 1)[0]
    n_under = read_words(host, sym(elf, "snes_fh_n_under"), 1)[0]
    if n_over and n_under:
        eo, ro = u64("snes_fh_emu_over"), u64("snes_fh_rest_over")
        eu, ru = u64("snes_fh_emu_under"), u64("snes_fh_rest_under")
        skipped = read_words(host, sym(elf, "snes_fh_skipped_over"), 1)[0]
        ms = lambda c: c / clk * 1e3
        print()
        print(f"{'':22}{'emulation':>12}{'present+audio':>15}{'total':>10}")
        print(f"  under the line ({n_under:5d}) {ms(eu/n_under):10.2f} ms"
              f"{ms(ru/n_under):13.2f} ms{ms((eu+ru)/n_under):8.2f} ms")
        print(f"  over  the line ({n_over:5d}) {ms(eo/n_over):10.2f} ms"
              f"{ms(ro/n_over):13.2f} ms{ms((eo+ro)/n_over):8.2f} ms")
        print(f"  difference           {ms(eo/n_over - eu/n_under):10.2f} ms"
              f"{ms(ro/n_over - ru/n_under):13.2f} ms")
        print(f"  frameskipped among the slow frames: {skipped}/{n_over}")

    print()
    peak = max(buckets) or 1
    for i, c in enumerate(buckets):
        if not c and i > line + 6:
            continue
        mark = "|" if i == int(line) else " "
        bar = "#" * int(60 * c / peak)
        print(f"{i * per_bucket_ms:6.2f} ms {mark} {c:6d} {bar}")


if __name__ == "__main__":
    main()
