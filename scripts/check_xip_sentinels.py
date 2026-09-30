#!/usr/bin/env python3
"""Fail the link if the Sega CD sentinel patcher would rewrite an instruction.

segacd.xip is linked at the sentinel address 0xDF000000 and cached into flash
wherever there is room. At load, main_segacd.c's patch_sentinels() walks the
blob, the RAM_EMU overlay and the ITCM section one aligned word at a time and
adds the real load offset to every word whose value falls inside
[0xDF000000, 0xDF000000 + size). It cannot tell a literal-pool pointer from an
instruction, so it trusts that no instruction word lands in that window.

That trust is layout luck. The second half of a Thumb-2 BL is 0b11J1_1Jxx...,
and 0xDF0x is one of them: a BL at a word-aligned address whose offset bits
happen to be right reads as 0xDF0xxxxx, and the patcher silently turns it into
a branch to nowhere. The build that shipped had none (checked 2026-09-30:
566 + 16 pointer words, 0 code words), but every change to what is linked
where re-rolls the dice, and moving hot code into ITCM is exactly such a change.

So check each build: every in-window word inside a Thumb code region (per the
$t/$d mapping symbols) is an error. Words in $d regions are literal pools and
tables, which is what the patcher is for.

usage: check_xip_sentinels.py <objdump> <elf>
"""
import bisect
import re
import subprocess
import sys

BASE = 0xDF000000
XIP_SECTIONS = (".xip_segacd", ".rodata_segacd")
SCANNED = (".xip_segacd", ".overlay_segacd", ".overlay_segacd_itc")


def main():
    objdump, elf = sys.argv[1], sys.argv[2]
    objcopy = objdump.replace("objdump", "objcopy")
    # objdump -t hides the ARM $t/$d mapping symbols; readelf -s lists them.
    readelf = objdump.replace("objdump", "readelf")
    try:
        headers = subprocess.run([objdump, "-h", elf], capture_output=True,
                                 text=True, check=True).stdout
        symtab = subprocess.run([readelf, "-sW", elf], capture_output=True,
                                text=True, check=True).stdout
        sections = subprocess.run([readelf, "-SW", elf], capture_output=True,
                                  text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"check_xip_sentinels: SKIPPED ({e})")
        return 0

    secs = {}
    for line in headers.splitlines():
        f = line.split()
        if len(f) >= 7 and f[0].isdigit():
            secs[f[1]] = (int(f[3], 16), int(f[2], 16))
    if not all(s in secs for s in XIP_SECTIONS):
        print("check_xip_sentinels: no Sega CD XIP in this build, nothing to check")
        return 0
    end = max(secs[s][0] + secs[s][1] for s in XIP_SECTIONS)
    size = end - BASE

    # A word is an instruction when it lies inside a function (FUNC symbol with
    # its size) and the last mapping symbol before it says Thumb. Data sections
    # carry no $d of their own, so the mapping symbols alone would call a .data
    # table that follows code "code" too. Only symbols of the section being
    # scanned count: every core's overlay shares these addresses, so another
    # core's $t at the same VMA says nothing about this one.
    sec_index = {}
    for line in sections.splitlines():
        m = re.match(r"^\s*\[\s*(\d+)\] (\S+)", line)
        if m:
            sec_index[m.group(2)] = m.group(1)
    maps, funcs = {}, {}
    for line in symtab.splitlines():
        m = re.match(r"^\s*\d+: ([0-9a-f]{8})\s+0 NOTYPE\s+LOCAL\s+DEFAULT\s+(\d+) \$([td])(\.\S*)?$", line)
        if m:
            maps.setdefault(m.group(2), []).append((int(m.group(1), 16), m.group(3)))
            continue
        f = re.match(r"^\s*\d+: ([0-9a-f]{8})\s+(\d+) FUNC\s+\S+\s+\S+\s+(\d+) ", line)
        if f and int(f.group(2)):
            a = int(f.group(1), 16) & ~1
            funcs.setdefault(f.group(3), []).append((a, a + int(f.group(2))))

    def classifier(sec):
        ndx = sec_index.get(sec)
        m = sorted(maps.get(ndx, []))
        fn = sorted(funcs.get(ndx, []))
        ms, fs = [a for a, _ in m], [a for a, _ in fn]

        def in_code(addr):
            j = bisect.bisect_right(fs, addr) - 1
            if j < 0 or addr >= fn[j][1]:
                return False
            i = bisect.bisect_right(ms, addr) - 1
            return i >= 0 and m[i][1] == "t"
        return in_code

    bad = []
    for sec in SCANNED:
        if sec not in secs or secs[sec][1] == 0:
            continue
        start = secs[sec][0]
        ndx = sec_index.get(sec)
        if not maps.get(ndx) or not funcs.get(ndx):
            # Nothing parsed means nothing could ever be "code": a pass here
            # would be the checker's blindness, not the image's health.
            print(f"check_xip_sentinels: no $t/FUNC symbols parsed for {sec}; "
                  "readelf output no longer matches, fix the parser")
            return 1
        in_code = classifier(sec)
        data = subprocess.run([objcopy, "-O", "binary", "--only-section=" + sec,
                               elf, "/dev/stdout"], capture_output=True, check=True).stdout
        for off in range(0, len(data) - 3, 4):
            v = int.from_bytes(data[off:off + 4], "little")
            if BASE <= (v & ~1) < BASE + size and in_code(start + off):
                bad.append((sec, start + off, v))
    for sec, addr, v in bad:
        print(f"check_xip_sentinels: {sec} {addr:#010x} holds {v:#010x}: an instruction "
              f"inside the sentinel window, patch_sentinels() would corrupt it")
    if bad:
        print("Move the function (or pad before it) so the word leaves the window.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
