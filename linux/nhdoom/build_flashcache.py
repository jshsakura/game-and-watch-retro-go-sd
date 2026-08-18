#!/usr/bin/env python3
"""Build a FALSE-POSITIVE-FREE doom1.flashcache.bin from >=2 raw host bakes.

The host bake classifies a cache word as a pointer purely by value range, so a
texture/data word whose value happens to land in the (ASLR-varying) host WAD or
cache range is mis-tagged. A TRUE pointer's value tracks the base, so its
NORMALIZED value (value - base) is identical across bakes; a false-positive data
word has a fixed value, so its normalized value differs. Keeping only offsets
whose normalized value is consistent across all bakes yields exactly the real
pointers, deterministically.

Output: fc_in[0]'s header + cache data + the filtered reloc table.
Usage: build_flashcache.py out.bin bakeA.bin bakeB.bin [bakeC.bin ...]
"""
import sys, struct

H = '<10I'  # magic ver cache_bytes host_wad_base host_wad_size host_cache_base host_cache_size n_reloc data_off reloc_off

def load(p):
    d = open(p, 'rb').read()
    f = struct.unpack_from(H, d, 0)
    assert f[0] == 0x4346484E, f"bad magic in {p}"
    nr, do, ro = f[7], f[8], f[9]
    rel = struct.unpack_from(f'<{nr}I', d, ro)
    return dict(raw=d, cb=f[2], hw=f[3], hws=f[4], hc=f[5], hcs=f[6],
                do=do, ro=ro, rel=rel)

def normmap(fc):
    """offset -> (type, normalized_value) for every reloc entry."""
    m = {}
    cache = fc['raw'][fc['do']:fc['do']+fc['cb']]
    for r in fc['rel']:
        off = r & ~1; typ = r & 1
        v = struct.unpack_from('<I', cache, off)[0]
        base = fc['hc'] if typ == 1 else fc['hw']
        m[off] = (typ, (v - base) & 0xFFFFFFFF)
    return m

def main():
    out, ins = sys.argv[1], sys.argv[2:]
    if len(ins) < 2:
        print("need >=2 bakes"); sys.exit(2)
    fcs = [load(p) for p in ins]
    maps = [normmap(f) for f in fcs]
    base = maps[0]
    keep = []
    dropped = 0
    for off, (typ, nv) in base.items():
        if all(off in m and m[off] == (typ, nv) for m in maps[1:]):
            keep.append(off | typ)
        else:
            dropped += 1
    keep.sort()
    fc = fcs[0]
    # rewrite header with the filtered reloc count; reloc table replaces the old.
    hdr = list(struct.unpack_from(H, fc['raw'], 0))
    hdr[7] = len(keep)                                   # n_reloc
    body = bytearray(fc['raw'][:fc['ro']])               # header + cache data
    struct.pack_into(H, body, 0, *hdr)
    body += struct.pack(f'<{len(keep)}I', *keep)
    open(out, 'wb').write(body)
    wad = sum(1 for k in keep if (k & 1) == 0)
    cac = sum(1 for k in keep if (k & 1) == 1)
    print(f"kept {len(keep)} relocs (WAD={wad} CACHE={cac}), dropped {dropped} "
          f"false-positives across {len(ins)} bakes -> {out} ({len(body)} bytes)")

main()
