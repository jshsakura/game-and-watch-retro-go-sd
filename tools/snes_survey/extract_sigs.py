#!/usr/bin/env python3
"""Extract SNES sound-driver ARAM signatures from VGMTrans scanner sources.

Why this exists
---------------
We are surveying a SNES ROM library to find how few sound-driver families cover
most of it (the "which drivers to HLE" question). VGMTrans already carries the
battle-tested ARAM byte patterns that identify each driver engine; hand-copying
hundreds of \\x.. bytes would be error-prone. This parses the real source
(src/main/formats/*Snes/*Scanner.cpp, zlib-licensed) into a C table the survey
harness scans apu->ram with.

Two definition shapes are parsed:
  1. Inline:   BytePattern Foo::ptnBar("\\xAA\\xBB", "x?", 2);
  2. NinSnes:  makePatchedBytePattern("\\xAA", "x", {{0, addr}});  -- the patched
               index is game-specific (an ARAM address), so we wildcard it.

Mask convention (VGMTrans): mask[i] == 'x' -> byte must equal; anything else
('?' or ' ') -> wildcard.

Output: snes_driver_sigs.h -- a flat array of {family, name, len, bytes[], mask[]}.
"""
import re
import sys
import os

SRC_DIR = os.path.join(os.path.dirname(__file__), "vgm_src")
OUT = os.path.join(os.path.dirname(__file__), "snes_driver_sigs.h")


def decode_c_string(literal_body):
    """Decode the inside of a C string literal to raw bytes."""
    out = bytearray()
    i = 0
    s = literal_body
    while i < len(s):
        c = s[i]
        if c == "\\":
            nxt = s[i + 1]
            if nxt == "x":
                # \xNN  (VGMTrans always uses exactly two hex digits)
                out.append(int(s[i + 2:i + 4], 16))
                i += 4
            elif nxt in "0123456789":
                j = i + 1
                while j < len(s) and j < i + 4 and s[j] in "01234567":
                    j += 1
                out.append(int(s[i + 1:j], 8) & 0xFF)
                i = j
            else:
                esc = {"n": 10, "t": 9, "r": 13, "0": 0, "\\": 92, '"': 34, "'": 39}
                out.append(esc.get(nxt, ord(nxt)))
                i += 2
        else:
            out.append(ord(c))
            i += 1
    return bytes(out)


# Pull consecutive string literals (adjacent literal concatenation) as ONE blob.
STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def concat_strings(segment):
    """Join all "..." literals in a comma-segment into one decoded byte blob."""
    parts = STRING_RE.findall(segment)
    blob = bytearray()
    for p in parts:
        blob += decode_c_string(p)
    return bytes(blob)


def split_top_level_commas(argstr):
    """Split a call's argument list on top-level commas (ignoring braces/brackets)."""
    segs, depth, cur = [], 0, []
    for ch in argstr:
        if ch in "{[(":
            depth += 1
        elif ch in "}])":
            depth -= 1
        if ch == "," and depth == 0:
            segs.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    segs.append("".join(cur))
    return segs


def balanced_args(text, open_idx):
    """Given index of '(', return (args_string, index_after_close)."""
    depth, i = 0, open_idx
    while i < len(text):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[open_idx + 1:i], i + 1
        i += 1
    raise ValueError("unbalanced")


INLINE_RE = re.compile(r"BytePattern\s+(\w+Scanner)::(\w+)\s*\(")
MAKE_FN_RE = re.compile(r"BytePattern\s+(\w+Scanner)::(make\w+)\s*\(")
PATCHED_RE = re.compile(r"makePatchedBytePattern\s*\(")


def family_of(scanner_class):
    # "RareSnesScanner" -> "Rare"; "NinSnesScanner" -> "Nin"
    name = scanner_class[:-len("Scanner")] if scanner_class.endswith("Scanner") else scanner_class
    return name[:-len("Snes")] if name.endswith("Snes") else name


def parse_inline(text, family_class_map):
    sigs = []
    for m in INLINE_RE.finditer(text):
        cls, name = m.group(1), m.group(2)
        args, _ = balanced_args(text, m.end() - 1)
        segs = split_top_level_commas(args)
        if len(segs) < 3:
            continue
        byts = concat_strings(segs[0])
        mask = concat_strings(segs[1])
        if not byts or len(mask) != len(byts):
            continue
        sigs.append((family_of(cls), name, byts, mask))
    return sigs


def parse_patched(text, scanner_class):
    """Parse makePatchedBytePattern(...) calls; wildcard the patched indices."""
    sigs = []
    for m in PATCHED_RE.finditer(text):
        # find the enclosing make... function name for a readable variant label
        fn = "patched"
        pre = text[:m.start()]
        fm = None
        for fmatch in MAKE_FN_RE.finditer(pre):
            fm = fmatch
        if fm:
            fn = fm.group(2)
        args, _ = balanced_args(text, m.end() - 1)
        segs = split_top_level_commas(args)
        if len(segs) < 3:
            continue
        byts = bytearray(concat_strings(segs[0]))
        mask = bytearray(concat_strings(segs[1]))
        if not byts or len(mask) != len(byts):
            continue
        # patch list: {{idx, val}, ...} -> those bytes are game-specific -> wildcard
        for pm in re.finditer(r"\{\s*(\d+)\s*,", segs[2]):
            idx = int(pm.group(1))
            if 0 <= idx < len(mask):
                mask[idx] = ord("?")
        sigs.append((family_of(scanner_class), fn, bytes(byts), bytes(mask)))
    return sigs


def main():
    all_sigs = []
    for fn in sorted(os.listdir(SRC_DIR)):
        if not fn.endswith(".cpp"):
            continue
        text = open(os.path.join(SRC_DIR, fn), encoding="utf-8", errors="replace").read()
        all_sigs += parse_inline(text, None)
        # NinSnesScannerPatterns.cpp carries the generated make...Pattern forms
        if "makePatchedBytePattern" in text:
            all_sigs += parse_patched(text, "NinSnesScanner")

    # de-dup identical (family,bytes,mask)
    seen, uniq = set(), []
    for fam, name, byts, mask in all_sigs:
        key = (fam, byts, mask)
        if key in seen:
            continue
        seen.add(key)
        uniq.append((fam, name, byts, mask))

    # a pattern of all-wildcards or too short is worthless / dangerous (matches noise)
    MIN_FIXED = 4  # require at least this many 'x' anchor bytes
    kept = [s for s in uniq if sum(1 for c in s[3] if c == ord("x")) >= MIN_FIXED]
    dropped = len(uniq) - len(kept)

    with open(OUT, "w") as f:
        f.write("/* AUTO-GENERATED by extract_sigs.py from VGMTrans scanner sources (zlib).\n")
        f.write(" * Do not edit by hand. Re-run: python3 tools/snes_survey/extract_sigs.py\n")
        f.write(" * Each sig: an ARAM code pattern identifying a SNES sound-driver family.\n")
        f.write(" * mask[i]=='x' -> bytes[i] must match; else wildcard. */\n")
        f.write("#ifndef SNES_DRIVER_SIGS_H\n#define SNES_DRIVER_SIGS_H\n\n")
        f.write("typedef struct { const char *family, *name; int len;\n")
        f.write("                 const unsigned char *bytes, *mask; } DriverSig;\n\n")
        f.write("static const DriverSig SNES_DRIVER_SIGS[] = {\n")
        for i, (fam, name, byts, mask) in enumerate(kept):
            bstr = "".join("\\x%02x" % b for b in byts)
            mstr = "".join(chr(c) if chr(c) in "x?" else "?" for c in mask)
            f.write('  { "%s", "%s", %d, (const unsigned char*)"%s", (const unsigned char*)"%s" },\n'
                    % (fam, name, len(byts), bstr, mstr))
        f.write("};\n")
        f.write("static const int SNES_DRIVER_SIG_COUNT = %d;\n\n" % len(kept))
        f.write("#endif\n")

    # family summary to stderr
    from collections import Counter
    byfam = Counter(s[0] for s in kept)
    print("families: %d, sigs kept: %d (dropped %d weak)" % (len(byfam), len(kept), dropped), file=sys.stderr)
    for fam, c in sorted(byfam.items(), key=lambda kv: -kv[1]):
        print("  %-14s %d" % (fam, c), file=sys.stderr)


if __name__ == "__main__":
    main()
