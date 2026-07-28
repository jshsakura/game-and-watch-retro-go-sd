#!/usr/bin/env python3
"""tools/tamapoke/verify_assets_dat.py must reject what the firmware rejects.

The verifier exists because the converter and the firmware disagreed about the
thumbnail record for every release and neither side could see it. A verifier that
has never rejected anything is the same kind of comfort those two had, so each case
below builds a container that is broken in ONE specific way and requires a finding.

Containers are built here rather than fixtured: the assets are CC BY-NC and never
enter this tree (docs/TAMAPOKE.md), so a test that needs real packs is a test that
cannot run in CI.
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
VERIFY = os.path.join(REPO, "tools", "tamapoke", "verify_assets_dat.py")

rc = 0


def ok(msg):
    print("  OK   %s" % msg)


def bad(msg):
    global rc
    rc = 1
    print("  FAIL %s" % msg)


def tpak(entries):
    """entries: [(name, blob)] -> a TPAK container."""
    out = b"TPAK" + struct.pack("<H", len(entries))
    for name, blob in entries:
        n = name.encode()
        out += struct.pack("<B", len(n)) + n + struct.pack("<I", len(blob))
    for _, blob in entries:
        out += blob
    return out


def pack(acts, pal_count=4):
    """acts: [(aid, w, h, nframes)] -> a TPK2 blob."""
    out = b"TPK2" + struct.pack("<B", len(acts)) + struct.pack("<H", pal_count)
    out += b"\x00\x00" * pal_count
    for aid, w, h, nf in acts:
        out += bytes([aid, w, h, nf])
        out += b"\x10\x00" * nf                 # ms per frame
        out += bytes([1]) * (w * h * nf)        # pixels, index 1
    return out


def thumbs(records):
    """records: [(w, h, pal_count)] -> a TPTH blob with correct offsets."""
    head = b"TPTH" + struct.pack("<H", len(records))
    table_len = 4 * len(records)
    off = len(head) + table_len
    offsets, bodies = [], b""
    for w, h, p in records:
        offsets.append(off)
        body = bytes([w, h, p]) + b"\x00\x00" * p + bytes([0xFF]) * (w * h)
        bodies += body
        off += len(body)
    return head + b"".join(struct.pack("<I", o) for o in offsets) + bodies


def run(container):
    with tempfile.NamedTemporaryFile(suffix=".dat", delete=False) as f:
        f.write(container)
        path = f.name
    try:
        p = subprocess.run([sys.executable, VERIFY, path],
                           capture_output=True, text=True)
        return p.returncode, p.stdout + p.stderr
    finally:
        os.unlink(path)


FULL = [(0, 8, 8, 2), (1, 8, 8, 2), (2, 8, 8, 2), (3, 8, 8, 2)]

print("=== tamapoke: the asset verifier rejects what the firmware rejects ===")

if not os.path.exists(VERIFY):
    print("SKIP: %s not present" % VERIFY)
    raise SystemExit(0)

# GREEN first: a well-formed container must pass, or every check below is noise.
code, out = run(tpak([("p001.bin", pack(FULL)), ("thumbs.bin", thumbs([(4, 5, 2)]))]))
if code != 0:
    bad("a well-formed container was rejected:\n%s" % out)
else:
    ok("a well-formed container passes")

# 1. The thumbnail record whose length disagrees with its own header. This is the
#    exact shape of the bug the firmware shipped: read the record by a fixed size
#    and you walk into the next species.
t = bytearray(thumbs([(4, 5, 2), (4, 5, 2)]))
t[6 + 4] = 0x20  # corrupt entry 1's offset (low byte) so the records overlap
code, out = run(tpak([("p001.bin", pack(FULL)), ("thumbs.bin", bytes(t))]))
if code == 0 or "record is" not in out:
    bad("a thumbnail whose record length disagrees with the next offset was accepted")
else:
    ok("a thumbnail record that overlaps its neighbour is rejected")

# 2. A thumbnail claiming more pixels than the file holds.
t = bytearray(thumbs([(4, 5, 2)]))
t[-1 - 0] = t[-1]          # no-op, keep length
t[6 + 4 - 4 + 0] = t[6]    # no-op
t2 = bytes(t[:-4])         # truncate the pixel block
code, out = run(tpak([("p001.bin", pack(FULL)), ("thumbs.bin", t2)]))
if code == 0:
    bad("a truncated thumbnail record was accepted")
else:
    ok("a truncated thumbnail record is rejected")

# 3. A pack with no PMD_IDLE: the firmware's last-resort fallback is missing, so
#    there is nothing it can draw.
code, out = run(tpak([("p001.bin", pack([(3, 8, 8, 2)])),
                      ("thumbs.bin", thumbs([(4, 5, 2)]))]))
if code == 0 or "PMD_IDLE" not in out:
    bad("a pack with no PMD_IDLE was accepted")
else:
    ok("a pack with no PMD_IDLE is rejected")

# 4. A pack over the firmware's 124 KB slot. It cannot load at all -- this is what
#    the whole rescale ladder in repack_tpk2.py exists to prevent.
big = pack([(0, 200, 200, 4)])          # 160000 bytes of pixels
code, out = run(tpak([("p001.bin", big), ("thumbs.bin", thumbs([(4, 5, 2)]))]))
if code == 0 or "slot" not in out:
    bad("a pack larger than PMD_BLOB_MAX was accepted")
else:
    ok("a pack over the 124 KB slot is rejected")

# 5. An action id past PMD_NACTS: parse_actions() refuses the whole pack, so the
#    species silently has no sprites at all.
code, out = run(tpak([("p001.bin", pack([(0, 8, 8, 2), (99, 8, 8, 2)])),
                      ("thumbs.bin", thumbs([(4, 5, 2)]))]))
if code == 0 or "PMD_NACTS" not in out:
    bad("an out-of-range action id was accepted")
else:
    ok("an action id past PMD_NACTS is rejected")

# 6. And the index/payload disagreement that would shift every blob.
c = bytearray(tpak([("p001.bin", pack(FULL)), ("thumbs.bin", thumbs([(4, 5, 2)]))]))
c += b"\x00" * 8                        # trailing bytes no index entry claims
code, out = run(bytes(c))
if code == 0:
    bad("a container with unclaimed trailing bytes was accepted")
else:
    ok("an index that does not account for the payload is rejected")

print()
print("FAILED" if rc else "PASSED")
raise SystemExit(rc)
