#!/usr/bin/env python3
"""Read a built tamapoke_assets.dat the way the FIRMWARE reads it, and fail on
anything the firmware would reject.

Why this exists: the thumbnail record layout was worked out by taking a shipped
.dat apart with a hex dump, because nothing on either side wrote it down. The
firmware had been reading those records as a fixed 24x24 block of raw bytes -- so
all 151 Pokedex thumbnails were drawn through the wrong palette, out of the wrong
offsets, for every release. A converter that does not read its own output the way
its consumer does cannot catch that, and neither can a unit test of the consumer:
both were self-consistent, and wrong about each other.

So the parsers below are deliberately written from the FIRMWARE's side --
tamapoke_sprites.cpp's PmdMon::load()/SdThumbs::get() and tamapoke_assets.cpp --
and not from the packers'. If the two ever disagree again, this says so at build
time instead of on someone's device.

    python3 verify_assets_dat.py <tamapoke_assets.dat>

Exits non-zero on any finding. Prints the action census either way: which PMD
actions the packs actually carry is not a detail, it is what the firmware's
animation fallbacks are sized against (see pickAct() in tamapoke_ui.cpp).
"""
import struct
import sys

# Mirrors of the firmware's own limits. Changing one of these here without
# changing it there is the whole class of bug this file exists to catch.
PMD_BLOB_MAX = 124 * 1024      # tamapoke_sprites.h
PMD_MAX_FRAMES = 24            # tamapoke_sprites.h
PMD_NACTS = 12                 # tamapoke_sprites.h
THUMBS_MAX = 56 * 1024         # tamapoke_sprites.h
PMD_TRANSPARENT_INDEX = 0xFF

ACT_NAMES = ["IDLE", "WALKL", "WALKR", "SLEEP", "EAT", "HURT",
             "ATTACK", "POSE", "HOP", "NOD", "BREATH", "SIT"]
PMD_IDLE = 0


class Findings:
    def __init__(self):
        self.errors = []
        self.warnings = []

    def error(self, msg):
        self.errors.append(msg)

    def warn(self, msg):
        self.warnings.append(msg)


def read_tpak(data, f):
    """TPAK: 'TPAK', u16 count, count x {u8 nameLen, name, u32 size}, then blobs
    in index order. Mirrors tamapoke_assets.cpp."""
    if data[:4] != b"TPAK":
        f.error("not a TPAK container (magic is %r)" % data[:4])
        return []
    count = struct.unpack_from("<H", data, 4)[0]
    p = 6
    entries = []
    for i in range(count):
        if p + 1 > len(data):
            f.error("index entry %d runs past the end of the file" % i)
            return entries
        n = data[p]
        p += 1
        name = data[p:p + n].decode("utf-8", "replace")
        p += n
        if p + 4 > len(data):
            f.error("index entry %r has no size field" % name)
            return entries
        size = struct.unpack_from("<I", data, p)[0]
        p += 4
        entries.append([name, size, 0])
    off = p
    for e in entries:
        e[2] = off
        off += e[1]
    if off != len(data):
        f.error("blobs end at %d but the file is %d bytes -- the index and the "
                "payload disagree" % (off, len(data)))
    return entries


def check_pack(name, blob, f, census):
    """TPK2: 'TPK2', u8 n_acts, u16 palCount, u16 pal[palCount], then per action
    {u8 aid, u8 w, u8 h, u8 nframes}, u16 ms[nframes], u8 px[w*h*nframes].
    Mirrors PmdMon::load() + parse_actions()."""
    if len(blob) < 7 or blob[:4] != b"TPK2":
        f.error("%s: not a TPK2 pack" % name)
        return
    if len(blob) > PMD_BLOB_MAX:
        f.error("%s: %d bytes, over the firmware's %d-byte slot -- it will not "
                "load at all" % (name, len(blob), PMD_BLOB_MAX))
    n_acts = blob[4]
    pal_count = struct.unpack_from("<H", blob, 5)[0]
    if pal_count > 256 or 7 + pal_count * 2 > len(blob):
        f.error("%s: palette of %d entries does not fit" % (name, pal_count))
        return

    p = 7 + pal_count * 2
    seen = set()
    for _ in range(n_acts):
        if p + 4 > len(blob):
            f.error("%s: action table runs past the end" % name)
            return
        aid, w, h, nf = blob[p], blob[p + 1], blob[p + 2], blob[p + 3]
        p += 4
        if aid >= PMD_NACTS:
            f.error("%s: action id %d is past PMD_NACTS (%d) -- the firmware "
                    "rejects the whole pack" % (name, aid, PMD_NACTS))
            return
        if nf > PMD_MAX_FRAMES:
            f.error("%s: %s has %d frames, over PMD_MAX_FRAMES (%d) -- the "
                    "firmware rejects the whole pack"
                    % (name, ACT_NAMES[aid], nf, PMD_MAX_FRAMES))
            return
        if w == 0 or h == 0 or nf == 0:
            f.error("%s: %s is %dx%d x%d frames" % (name, ACT_NAMES[aid], w, h, nf))
            return
        need = nf * 2 + w * h * nf
        if p + need > len(blob):
            f.error("%s: %s claims %d bytes of frames and only %d remain"
                    % (name, ACT_NAMES[aid], need, len(blob) - p))
            return
        # Every non-transparent index must name a colour the pack ships. The
        # firmware treats anything >= palCount as transparent, so this is a
        # silent-hole check rather than a crash check.
        frames = blob[p + nf * 2:p + need]
        bad = {v for v in set(frames) if v >= pal_count and v != PMD_TRANSPARENT_INDEX}
        if bad:
            f.warn("%s: %s uses %d index(es) outside its palette (%s) -- drawn as "
                   "transparent" % (name, ACT_NAMES[aid], len(bad),
                                    ",".join(str(v) for v in sorted(bad)[:4])))
        p += need
        seen.add(aid)
        census[aid] = census.get(aid, 0) + 1

    # The firmware's last-resort fallback is PMD_IDLE; a pack without it has
    # nothing to draw at all.
    if PMD_IDLE not in seen:
        f.error("%s: no PMD_IDLE -- the firmware has no fallback to draw" % name)


def check_thumbs(blob, f):
    """TPTH: 'TPTH', u16 count, u32 offset[count], then per entry
    {u8 w, u8 h, u8 palCount, u16 pal[palCount], u8 px[w*h]} with 0xFF
    transparent. Mirrors SdThumbs::load()/get().

    This is the record the firmware read as a fixed 24x24 raw block for every
    release: it drew the w/h/palette header as if those bytes were pixels, ran off
    the end of each record into the next species, and looked the result up in the
    ASCII sprite palette. Nothing on either side had written the layout down."""
    if len(blob) > THUMBS_MAX:
        f.error("thumbs.bin is %d bytes, over the firmware's %d-byte buffer"
                % (len(blob), THUMBS_MAX))
    if len(blob) < 6 or blob[:4] != b"TPTH":
        f.error("thumbs.bin is not a TPTH file")
        return
    count = struct.unpack_from("<H", blob, 4)[0]
    if 6 + 4 * count > len(blob):
        f.error("thumbs.bin offset table (%d entries) does not fit" % count)
        return
    offs = [struct.unpack_from("<I", blob, 6 + 4 * i)[0] for i in range(count)]
    for i, off in enumerate(offs):
        dex = i + 1
        if off + 3 > len(blob):
            f.error("thumb #%03d: header at %d is past the end" % (dex, off))
            continue
        w, h, pal = blob[off], blob[off + 1], blob[off + 2]
        if not w or not h or not pal:
            f.error("thumb #%03d: %dx%d with %d palette entries" % (dex, w, h, pal))
            continue
        need = 3 + 2 * pal + w * h
        if off + need > len(blob):
            f.error("thumb #%03d: needs %d bytes, only %d remain"
                    % (dex, need, len(blob) - off))
            continue
        # The record must be exactly as long as its own arithmetic says, or the
        # next entry's offset is pointing into this one's pixels.
        nxt = offs[i + 1] if i + 1 < count else len(blob)
        if off + need != nxt:
            f.error("thumb #%03d: record is %d bytes by its own header but the "
                    "next entry starts %d bytes later" % (dex, need, nxt - off))


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    data = open(path, "rb").read()

    f = Findings()
    entries = read_tpak(data, f)
    census = {}
    packs = 0
    thumbs_seen = False

    for name, size, off in entries:
        blob = data[off:off + size]
        if name == "thumbs.bin":
            thumbs_seen = True
            check_thumbs(blob, f)
        elif name.startswith("p") and name.endswith(".bin") and name[1:-4].lstrip("s").isdigit():
            packs += 1
            check_pack(name, blob, f, census)

    if not packs:
        f.error("no sprite packs in the container")
    if not thumbs_seen:
        f.warn("no thumbs.bin -- the Pokedex will fall back to flash sprites")

    print("%s: %d entries, %d sprite packs, %.1f MB"
          % (path, len(entries), packs, len(data) / 1048576.0))
    if packs:
        print("action census (what the firmware's fallbacks are sized against):")
        for aid in range(PMD_NACTS):
            n = census.get(aid, 0)
            flag = "" if n == packs else "   <- not in every pack"
            print("  PMD_%-7s %4d/%d%s" % (ACT_NAMES[aid], n, packs, flag))

    for w in f.warnings:
        print("  warn  %s" % w)
    for e in f.errors[:40]:
        print("  FAIL  %s" % e)
    if len(f.errors) > 40:
        print("  ... and %d more" % (len(f.errors) - 40))

    if f.errors:
        print("FAILED: %d finding(s) the firmware would trip over" % len(f.errors))
        return 1
    print("OK: the firmware's own parsers accept this container")
    return 0


if __name__ == "__main__":
    sys.exit(main())
