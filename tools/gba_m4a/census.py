#!/usr/bin/env python3
"""How many DIFFERENT builds of M4A's mixer are out there, and who has which?

    ./census.py /path/to/roms            what is in the corpus
    ./census.py /path/to/roms --csv      one row per cart, for a database

This is the generator. It answers the only question that actually scales:
**not "which games", but "which variants"**. M4A is a library — the same bytes
in every cart that links the same build of it — so one transliteration covers
every game that carries it, for ever, with no per-game data and no per-game work.
633 carts turned out to hold **six** distinct mixers.

Nothing here emulates anything. The block sits verbatim in the ROM (the sound
library copies it to IWRAM at boot), and it is delimited by two fingerprints no
ordinary code carries:

    entry:  str r8, [sp]                          e58d8000
    exit:   ldr r8, [sp]                          e59d8000
            add r0, pc, #1                        e28f0001
            bx  r0                                e12fff10

Find the exit, walk back to the nearest entry, and that IS the block. Hash it.

The hash is what `m4a_hle.c` matches on, so a hash that appears here and not
there is a variant nobody has transliterated yet — and the count of carts behind
it is exactly how much it is worth. That is the whole priority list, and it is
data rather than opinion.

Sibling to `idlefind`, and the same shape of artifact: sweep a corpus, emit a
table, copy the table into the firmware, never hand-edit it. The two tools answer
the same question — *will this cart run on the real hardware?* — from opposite
ends, and neither answer is complete alone. A cart with no idle loop but a known
mixer is a third cheaper than the idle sweep believes.
"""
import glob
import hashlib
import os
import sys
from collections import defaultdict

ENTRY = bytes.fromhex("00808de5")
EXIT = bytes.fromhex("00809de5") + bytes.fromhex("01008fe2") + bytes.fromhex("10ff2fe1")

# The variants m4a_hle.c can already run. Keep in step with m4a_variants[].
KNOWN = {
    "72315cec4e045f69": "m4a-soundmainram-mono",
    "3237f8b3":         "m4a-soundmainram-stereo",
    "ffd1701f04cd":     "m4a-soundmainram-stereo2",
    "64c146fc6c75":     "m4a-soundmainram-stereo3",
}


def known_name(h):
    for prefix, name in KNOWN.items():
        if h.startswith(prefix):
            return name
    return None


def find_mixer(rom):
    """(sha1, offset, length) of the first M4A mixer in this ROM, or None."""
    pos = 0
    while True:
        e = rom.find(EXIT, pos)
        if e < 0:
            return None
        pos = e + 4
        if e % 4:
            continue
        start = -1
        for back in range(4, 4096, 4):
            if e - back < 0:
                break
            if rom[e - back:e - back + 4] == ENTRY:
                start = e - back
                break
        if start < 0:
            continue
        blk = rom[start:e + 12]
        return hashlib.sha1(blk).hexdigest(), start, len(blk)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    romdir = sys.argv[1]
    as_csv = "--csv" in sys.argv

    files = sorted(glob.glob(os.path.join(romdir, "*.gba")))
    variants = defaultdict(list)
    nomixer = []
    rows = []

    for f in files:
        try:
            rom = open(f, "rb").read()
        except OSError:
            continue
        name = os.path.basename(f)
        code = rom[0xAC:0xB0].decode("latin1") if len(rom) > 0xB0 else "????"
        got = find_mixer(rom)
        if not got:
            nomixer.append(name)
            rows.append((name, code, "", "", "none"))
            continue
        h, off, size = got
        variants[h].append(name)
        rows.append((name, code, h[:12], str(size), known_name(h) or "UNTRANSLITERATED"))

    if as_csv:
        print("game,code,mixer_sha1,block_bytes,variant")
        for r in rows:
            print(",".join('"%s"' % c if "," in c else c for c in r))
        return 0

    n = len(files)
    print("roms: %d   distinct mixer variants: %d   no mixer found: %d\n"
          % (n, len(variants), len(nomixer)))
    print("%-14s %6s %8s  %5s  %s" % ("mixer", "carts", "cum", "bytes", "status"))
    cum = 0
    for h, games in sorted(variants.items(), key=lambda kv: -len(kv[1])):
        cum += len(games)
        name = known_name(h)
        rom = open(os.path.join(romdir, games[0]), "rb").read()
        size = find_mixer(rom)[2]
        print("%-14s %6d %7.1f%%  %5d  %s"
              % (h[:12], len(games), 100.0 * cum / n, size,
                 name or "*** not transliterated — %d carts waiting ***" % len(games)))
        print("%-14s        %8s  %5s  e.g. %s" % ("", "", "", games[0][:52]))
    print("\ncovered by a transliteration we have: %d of %d (%.1f%%)"
          % (sum(len(g) for h, g in variants.items() if known_name(h)), n,
             100.0 * sum(len(g) for h, g in variants.items() if known_name(h)) / n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
