#!/usr/bin/env python3
"""Fail the link if /bios/logo.bin's index space drifts from the RG_LOGO_* enum.

rg_get_logo() maps enum value -> blob index as (value - INT_LOGO_COUNT), and the
blob is a raw objcopy of .sdcard_logo, so the ONLY thing that keeps a tab from
drawing some other system's wordmark -- or a 40px pad into an 18px header slot,
which odroid_overlay_draw_logo() then writes past the framebuffer -- is that the
enum order in bitmaps.h equals the link order of the LOGO_DATA structs.

That equality has now silently broken twice:
  - 0722 upstream merge: three logos shifted; caught by a manual `nm -n` diff.
  - 0727 upstream merge: header_lynx/pad_lynx landed at blob 18/36 while the
    enum kept them at sd44 / the colour-only tail. Every fork tab header from
    PICO-8 on served its predecessor; HEADER_WSWAN and HEADER_PCECD served
    32-40px pads into the 18px header bar and the unclipped draw corrupted the
    RAM after the framebuffer (grid view / favorites / settings crashes).

It replaces scripts/check_logo_order.py (0722), which was written for the same
purpose and passed the 0727 break without a murmur -- "OK (32 headers, enum ==
link order)" against the header that was crashing devices. Worth knowing why,
because the blind spot is easy to rebuild: that script collected only `header_*`
symbols and compared the two lists pairwise, so it proved the headers were in
order RELATIVE TO EACH OTHER. What shifted in 0727 was their ABSOLUTE index,
because upstream inserted a PAD (pad_lynx) ahead of them -- and pads were not in
either list. A gate on relative order cannot see an insertion outside the set it
looks at. This one checks absolute position for every LOGO_DATA symbol there is.

Rules checked:
  1. Every enum entry that has a LOGO_DATA struct must sit at the blob index
     its enum value implies (value - INT_LOGO_COUNT == position in link order).
  2. Colour-only entries (no struct) may exist ONLY after the last backed one:
     an unbacked slot in the middle shifts everything behind it.

If the toolchain's nm is missing this SKIPS LOUDLY and exits 0 -- a safety net
must not be the thing that breaks the build (see CLAUDE.md).
"""
import re
import subprocess
import sys

INT_LOGO_COUNT = 3  # logo_rgo / logo_rgw / logo_gnw live in internal flash

# Enum names whose backing symbol does not follow the header_/pad_ convention.
VENDOR = {
    "RG_LOGO_COLECO": "logo_coleco",
    "RG_LOGO_NINTENDO": "logo_nintendo",
    "RG_LOGO_SEGA": "logo_sega",
    "RG_LOGO_PCE": "logo_pce",
    "RG_LOGO_MICROSOFT": "logo_microsoft",
    "RG_LOGO_WATARA": "logo_watara",
    "RG_LOGO_ATARI": "logo_atari",
    "RG_LOGO_AMSTRAD": "logo_amstrad",
    "RG_LOGO_TAMA": "logo_tama",
}
# Enum entries that never index the blob: intflash-served or sentinels.
NON_BLOB = {"RG_LOGO_EMPTY", "RG_LOGO_RGO", "RG_LOGO_RGW", "RG_LOGO_GNW"}


def enum_order(bitmaps_h):
    txt = open(bitmaps_h).read()
    m = re.search(r"enum\s*\{(.*?)\};", txt, re.S)
    if not m:
        sys.exit("logo-index-check: FAILED to find the RG_LOGO enum in %s" % bitmaps_h)
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    val, out = -1, []
    for tok in body.split(","):
        tok = tok.strip()
        if not tok:
            continue
        if "=" in tok:
            name, v = [t.strip() for t in tok.split("=")]
            val = int(v, 0)
        else:
            name = tok
            val += 1
        out.append((val, name))
    return out


def symbol_for(name):
    if name in VENDOR:
        return VENDOR[name]
    for pref, sym in (("RG_LOGO_HEADER_", "header_"), ("RG_LOGO_PAD_", "pad_")):
        if name.startswith(pref):
            return sym + name[len(pref):].lower()
    return None


def blob_order(nm, elf):
    try:
        out = subprocess.run([nm, "-n", elf], capture_output=True, text=True, check=True).stdout
    except (FileNotFoundError, subprocess.CalledProcessError) as e:
        print("logo-index-check: SKIPPED (cannot run %s: %s) -- alignment NOT verified" % (nm, e))
        sys.exit(0)
    # .sdcard_logo symbols are the R/T constants in the extflash logo LMA range;
    # take every header_*/pad_*/logo_* data symbol in address order and drop the
    # three intflash ones, which sort into a different address range anyway.
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        _, typ, sym = parts
        if typ.upper() not in ("R", "D", "T"):
            continue
        if re.fullmatch(r"(header|pad|logo)_[a-z0-9_]+", sym) and sym not in (
            "logo_rgo", "logo_rgw", "logo_gnw"
        ):
            syms.append(sym)
    return syms


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: check_logo_index_alignment.py <nm> <elf> <bitmaps.h>")
    nm, elf, bitmaps_h = sys.argv[1:]
    blob = blob_order(nm, elf)
    if not blob:
        print("logo-index-check: SKIPPED (no .sdcard_logo symbols found) -- alignment NOT verified")
        return
    failures = []
    seen_unbacked = None
    for val, name in enum_order(bitmaps_h):
        if name in NON_BLOB:
            continue
        sym = symbol_for(name)
        idx = val - INT_LOGO_COUNT
        if sym in blob:
            if seen_unbacked is not None:
                failures.append(
                    "%s is blob-backed (%s) but sits AFTER colour-only %s -- "
                    "backed entries must precede every unbacked one" % (name, sym, seen_unbacked)
                )
            actual = blob.index(sym)
            if actual != idx:
                failures.append(
                    "%s: enum says blob[%d], link order puts %s at blob[%d]" % (name, idx, sym, actual)
                )
        else:
            if seen_unbacked is None:
                seen_unbacked = name
    enum_backed = [symbol_for(n) for v, n in enum_order(bitmaps_h)
                   if n not in NON_BLOB and symbol_for(n) in blob]
    for sym in blob:
        if sym not in enum_backed:
            failures.append("blob has %s but no RG_LOGO_* enum entry maps to it" % sym)
    if failures:
        print("logo-index-check: FAILED -- /bios/logo.bin indices no longer match bitmaps.h:")
        for f in failures:
            print("  " + f)
        print("Fix: reorder the RG_LOGO_* enum (or the LOGO_DATA structs) until "
              "enum value - %d equals each struct's position in `nm -n` order." % INT_LOGO_COUNT)
        sys.exit(1)
    print("logo-index-check: OK (%d blob logos aligned with bitmaps.h)" % len(blob))


if __name__ == "__main__":
    main()
