#!/usr/bin/env python3
"""Classify a snes_survey.tsv into a sound-driver histogram.

Input: the TSV run_survey.sh writes (rom \\t status \\t lit \\t fam:sig,fam:sig,...).
Output: a driver-family histogram + N-SPC sub-variant breakdown + the unmatched
and crashed ROMs listed by name (so the tail can be eyeballed).

The survey emits every signature that hit, including shared idioms (LoadDIR /
SetDIR / instrument-table reads) that several engines share and that therefore
cannot decide a family on their own. We split those out as WEAK and let only the
engine-core signatures (section-pointer walk, vcmd dispatch, note dispatch, song
load) name the driver.

N-SPC (Nintendo's Kankichi engine) is one HLE target even though many studios
forked it, so every "Nin" variant rolls up to N-SPC; the sub-variant (which fork)
is reported separately.
"""
import sys
from collections import Counter, defaultdict

# signature-name substrings that are shared idioms, not engine identity
WEAK = ("LoadDIR", "SetDIR", "ReadSRCNTable", "LoadInstrTable", "WriteVolume")

# Nin sub-variant label from the signature name (VGMTrans NinSnesScanner comments)
def nspc_variant(signame):
    for key, label in (
        ("GD3", "Konami(GD3)"), ("YSFR", "Tose"), ("Ys4", "Falcom-Ys4"),
        ("YI", "YoshisIsland"), ("SMW", "SMW-era"), ("TS", "Tose-alt"),
        ("HE4", "Intelli-HE4"), ("FE3", "Intelli-FE3"), ("FE4", "Intelli-FE4"),
        ("Intelli", "Intelli"), ("LEM", "Lemmings"), ("KSS", "KSS"),
    ):
        if key in signame:
            return label
    return "standard"


def is_weak(signame):
    return any(w in signame for w in WEAK)


def classify(match_field):
    """Return (family, detail) for one ROM's match list."""
    if match_field in ("-", ""):
        return ("UNMATCHED", "")
    pairs = []
    for tok in match_field.split(","):
        fam, _, sig = tok.partition(":")
        pairs.append((fam, sig))

    strong = [(f, s) for (f, s) in pairs if not is_weak(s)]
    nin_strong = [s for (f, s) in strong if f == "Nin"]
    if nin_strong:
        # pick the most specific (non-standard) variant if present
        variants = [nspc_variant(s) for s in nin_strong]
        v = next((x for x in variants if x != "standard"), "standard")
        return ("N-SPC", v)

    non_nin = [f for (f, s) in strong if f != "Nin"]
    if non_nin:
        c = Counter(non_nin)
        top, topn = c.most_common(1)[0]
        ties = [f for f, k in c.items() if k == topn]
        return (top, "ambiguous:" + "/".join(sorted(ties)) if len(ties) > 1 else "")

    # only weak idioms hit -> engine unknown to us but audibly a driver
    weakfams = sorted({f for (f, s) in pairs})
    return ("WEAK_ONLY", "/".join(weakfams))


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/snes_survey.tsv"
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 4:
            continue
        rows.append(parts)

    total = len(rows)
    fam_hist = Counter()
    nspc_variants = Counter()
    unmatched, weak_only, crashes, loadfail = [], [], [], []

    for parts in rows:
        rom, status, lit, matches = parts[0], parts[1], parts[2], parts[3]
        if status == "LOAD_FAIL":
            fam_hist["(LOAD_FAIL:mapper)"] += 1; loadfail.append(rom); continue
        if status == "BOOT_CRASH":
            fam_hist["(BOOT_CRASH)"] += 1; crashes.append(rom); continue
        if status in ("NO_APU", "OPEN_FAIL", "READ_FAIL"):
            fam_hist["(" + status + ")"] += 1; continue
        fam, detail = classify(matches)
        fam_hist[fam] += 1
        if fam == "N-SPC":
            nspc_variants[detail] += 1
        elif fam == "UNMATCHED":
            unmatched.append((rom, lit))
        elif fam == "WEAK_ONLY":
            weak_only.append((rom, detail))

    def pct(n): return "%5.1f%%" % (100.0 * n / total) if total else "  -  "

    print("=" * 60)
    print("SNES sound-driver survey  (%d ROMs)" % total)
    print("=" * 60)
    print("\nDriver family histogram:")
    for fam, n in fam_hist.most_common():
        bar = "#" * n
        print("  %-22s %4d  %s  %s" % (fam, n, pct(n), bar))

    bootable = sum(n for f, n in fam_hist.items() if not f.startswith("("))
    nspc = fam_hist.get("N-SPC", 0)
    if bootable:
        print("\n  N-SPC share of bootable+classified: %.1f%% (%d/%d)"
              % (100.0 * nspc / bootable, nspc, bootable))

    if nspc_variants:
        print("\nN-SPC sub-variants (which fork of Nintendo's engine):")
        for v, n in nspc_variants.most_common():
            print("  %-22s %4d" % (v, n))

    if weak_only:
        print("\nWEAK_ONLY (only shared idioms hit -> engine not in our strong sigs):")
        for rom, fams in weak_only:
            print("  %-45s [%s]" % (rom[:45], fams))

    if unmatched:
        print("\nUNMATCHED (no signature at all -> unknown driver or too few boot frames):")
        for rom, lit in unmatched:
            print("  %-45s lit=%s" % (rom[:45], lit))

    if crashes:
        print("\nBOOT_CRASH (%d) -- core could not run these to classify:" % len(crashes))
        for rom in crashes:
            print("  %s" % rom[:60])
    if loadfail:
        print("\nLOAD_FAIL (%d) -- rejected mapper (SA-1/SuperFX/etc.):" % len(loadfail))
        for rom in loadfail:
            print("  %s" % rom[:60])


if __name__ == "__main__":
    main()
