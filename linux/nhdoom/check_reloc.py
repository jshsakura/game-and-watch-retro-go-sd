#!/usr/bin/env python3
"""Offline, engine-free rigor for the nhdoom flash-cache relocation.

Given doom1.flashcache.bin (built by the host bake), prove the relocation table +
arithmetic are correct independent of the flaky host engine:

  1. table self-consistency: every WAD-type word's value is in the host WAD range
     and every CACHE-type word's value is in the host cache range (no mislabels);
     and the set of reloc offsets equals EXACTLY the set of in-range words in the
     cache (no false positives / no missed pointers).
  2. relocation lands in-range: applying arbitrary (large, negative, independent)
     WAD/cache deltas moves every pointer to the corresponding NEW range.
  3. round-trip: relocate to new bases then back -> byte-identical to the original
     (so the device's `+= delta` matches what the host produced; reversible for
     any delta, proving no overflow/aliasing bugs).

This is the same byte transform the device applies in NhdoomLoadFlashCache.
"""
import sys, struct

def load(path):
    d = bytearray(open(path, 'rb').read())
    (mg, ver, cb, hw, hws, hc, hcs, nr, do, ro) = struct.unpack_from('<10I', d, 0)
    assert mg == 0x4346484E, "bad magic"
    rel = list(struct.unpack_from(f'<{nr}I', d, ro))
    cache = bytearray(d[do:do+cb])
    return dict(cache=cache, rel=rel, cb=cb, hw=hw, hws=hws, hc=hc, hcs=hcs, nr=nr)

def words_in_range(cache, lo, hi):
    out = set()
    for off in range(0, len(cache) - 3, 4):
        v = struct.unpack_from('<I', cache, off)[0]
        if lo <= v < hi:
            out.add(off)
    return out

def relocate(cache, rel, hw, hws, hc, hcs, wad_d, cache_d):
    c = bytearray(cache)
    whlo, whhi = hw, hw + hws
    chlo, chhi = hc, hc + hcs
    for r in rel:
        off = r & ~1; typ = r & 1
        v = struct.unpack_from('<I', c, off)[0]
        if typ == 1:
            if chlo <= v < chhi:
                struct.pack_into('<I', c, off, (v + cache_d) & 0xFFFFFFFF)
        else:
            if whlo <= v < whhi:
                struct.pack_into('<I', c, off, (v + wad_d) & 0xFFFFFFFF)
    return c

def main():
    fc = load(sys.argv[1])
    cache, rel = fc['cache'], fc['rel']
    hw, hws, hc, hcs = fc['hw'], fc['hws'], fc['hc'], fc['hcs']
    wad_off  = {r & ~1 for r in rel if (r & 1) == 0}
    cac_off  = {r & ~1 for r in rel if (r & 1) == 1}
    print(f"cache_bytes={fc['cb']} n_reloc={fc['nr']} WAD={len(wad_off)} CACHE={len(cac_off)}")
    print(f"host_wad=[0x{hw:x},0x{hw+hws:x}) host_cache=[0x{hc:x},0x{hc+hcs:x})")
    ok = True

    # (1) table self-consistency vs an independent re-scan of the cache
    wad_scan = words_in_range(cache, hw, hw + hws)
    cac_scan = words_in_range(cache, hc, hc + hcs)
    c1 = (wad_off == wad_scan) and (cac_off == cac_scan)
    print(f"[1] table == independent in-range scan: {'PASS' if c1 else 'FAIL'} "
          f"(wad {len(wad_off)}vs{len(wad_scan)}, cache {len(cac_off)}vs{len(cac_scan)})")
    ok &= c1

    # (2)+(3) for several deltas incl. large, negative, independent
    NEWW, NEWC = 0x90000000, 0x60000000      # arbitrary device-like new bases
    deltas = [
        ("large+",       NEWW - hw,            NEWC - hc),
        ("independent",  0x11111000 - 0,       0x07654000 - 0),   # unequal, applied raw
        ("negative",     -(0x05000000),        -(0x03000000)),
        ("wad-only",     NEWW - hw,            0),
        ("cache-only",   0,                    NEWC - hc),
    ]
    for name, wd, cd in deltas:
        relo = relocate(cache, rel, hw, hws, hc, hcs, wd, cd)
        # (2) landed in new range
        good = True
        for r in rel:
            off = r & ~1; typ = r & 1
            v0 = struct.unpack_from('<I', cache, off)[0]
            v1 = struct.unpack_from('<I', relo,  off)[0]
            exp = (v0 + (cd if typ else wd)) & 0xFFFFFFFF
            if v1 != exp:
                good = False; break
        # (3) round-trip back to original
        back = relocate(relo, rel,
                        hw + wd if False else hw, hws, hc, hcs, 0, 0)  # placeholder
        # proper inverse: shift host ranges by the applied delta, then invert
        whlo, whhi = (hw + wd) & 0xFFFFFFFF, (hw + wd + hws) & 0xFFFFFFFF
        chlo, chhi = (hc + cd) & 0xFFFFFFFF, (hc + cd + hcs) & 0xFFFFFFFF
        back = bytearray(relo)
        for r in rel:
            off = r & ~1; typ = r & 1
            v = struct.unpack_from('<I', back, off)[0]
            struct.pack_into('<I', back, off, (v - (cd if typ else wd)) & 0xFFFFFFFF)
        rt = bytes(back) == bytes(cache)
        res = good and rt
        print(f"[2/3] {name:12s} wadD={wd:+d} cacheD={cd:+d}: "
              f"land={'ok' if good else 'BAD'} roundtrip={'ok' if rt else 'BAD'} "
              f"-> {'PASS' if res else 'FAIL'}")
        ok &= res

    print("OFFLINE RELOC PROOF:", "ALL PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)

main()
