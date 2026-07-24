#!/usr/bin/env python3
"""Generates Core/Src/porting/cps1/cps1_romset.c from tools/cps1_romsets.json.

Two programs need the same CPS-1 chip table: this firmware, which assigns each
chip to a slot at load time, and the library app, which validates a romset at
upload time. Keeping two hand-written copies in step is a discipline, and the
failure mode when the discipline slips is a game that loads cleanly and renders
wrong -- the exact failure CRC identification exists to prevent. So there is one
table, in JSON, and the C is generated from it.

  tools/gen_cps1_romset.py              rewrite the .c
  tools/gen_cps1_romset.py --check      exit 1 if the .c is stale (used by tests)

Deliberately emits no filenames. They live in the JSON as comments for
cross-referencing MAME, and nothing on the device may look a chip up by one.
"""
import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
JSON_PATH = ROOT / "tools" / "cps1_romsets.json"
C_PATH = ROOT / "Core" / "Src" / "porting" / "cps1" / "cps1_romset.c"

HEADER = '''/*
 * GENERATED FILE -- do not edit.
 *
 * Source:    tools/cps1_romsets.json
 * Generator: tools/gen_cps1_romset.py
 *
 * Regenerate with `python3 tools/gen_cps1_romset.py`. tests/run.sh fails if this
 * file and the JSON disagree, so an edit here is reverted by the next test run
 * rather than silently kept.
 *
 * The JSON is the single source of truth and is checked into the library app's
 * repository byte-for-byte identically; its sha256 is recorded in
 * docs/CPS1_LIBRARY_CONTRACT.md so a divergent copy is detectable from either
 * side alone. See cps1_romset.h for why chips are keyed by hash and never by
 * filename.
 */
#include <stddef.h>

#include "cps1_romset.h"

/* The firmware's shared table-driven CRC32 (Core/Src/porting/crc32.c). Declared
 * here rather than pulled in via a header because the only header that declares
 * it lives in the host build tree. */
extern unsigned int crc32_le(unsigned int crc, unsigned char const *buf, unsigned int len);

uint32_t cps1_crc32(const uint8_t *data, uint32_t len)
{
    /* crc32_le() already inverts on the way in and on the way out, so a 0 seed
     * gives the standard zlib/MAME CRC32 with nothing added. Wrapping it in
     * another pair of inversions -- which its name invites -- cancels the inner
     * ones and produces a number that matches no MAME hash. */
    return crc32_le(0, (unsigned char const *)data, len);
}

const cps1_romset_t cps1_romsets[] = {
'''

FOOTER = '''};

const unsigned cps1_romset_count = sizeof(cps1_romsets) / sizeof(cps1_romsets[0]);

static unsigned count_missing(const cps1_romset_t *set, const uint32_t *crcs, unsigned count);

static int find_crc(const uint32_t *crcs, unsigned count, uint32_t want)
{
    for (unsigned i = 0; i < count; i++)
        if (crcs[i] == want)
            return (int)i;
    return -1;
}

int cps1_romset_resolve(const cps1_romset_t *set, const uint32_t *crcs, unsigned count,
                         int prg_index[CPS1_ROMSET_PRG_CHIPS],
                         int gfx_index[CPS1_ROMSET_GFX_CHIPS])
{
    if (set == NULL || crcs == NULL || prg_index == NULL || gfx_index == NULL)
        return -1;

    int prg[CPS1_ROMSET_PRG_CHIPS], gfx[CPS1_ROMSET_GFX_CHIPS];
    unsigned missing = 0;

    for (unsigned i = 0; i < CPS1_ROMSET_PRG_CHIPS; i++) {
        prg[i] = find_crc(crcs, count, set->prg_crc[i]);
        if (prg[i] < 0)
            missing++;
    }
    for (unsigned i = 0; i < CPS1_ROMSET_GFX_CHIPS; i++) {
        gfx[i] = find_crc(crcs, count, set->gfx_crc[i]);
        if (gfx[i] < 0)
            missing++;
    }
    if (missing != 0)
        return -1;

    /* Only write the caller's slots once the whole set is known present, so a
     * failed resolve never leaves half-filled indices behind. */
    for (unsigned i = 0; i < CPS1_ROMSET_PRG_CHIPS; i++)
        prg_index[i] = prg[i];
    for (unsigned i = 0; i < CPS1_ROMSET_GFX_CHIPS; i++)
        gfx_index[i] = gfx[i];
    return 0;
}

int cps1_romset_resolve_sound(const cps1_romset_t *set, const uint32_t *crcs, unsigned count,
                               int *audio_index,
                               int qsound_index[CPS1_ROMSET_QSOUND_CHIPS])
{
    if (audio_index != NULL)
        *audio_index = -1;
    if (qsound_index != NULL)
        for (unsigned i = 0; i < CPS1_ROMSET_QSOUND_CHIPS; i++)
            qsound_index[i] = -1;

    if (set == NULL || crcs == NULL || audio_index == NULL || qsound_index == NULL)
        return 0;
    if (set->audio_crc == 0u)   /* this romset lists no sound chips */
        return 0;

    int aud = find_crc(crcs, count, set->audio_crc);
    if (aud < 0)
        return 0;               /* container predates sound -- play silently */

    int qs[CPS1_ROMSET_QSOUND_CHIPS];
    for (unsigned i = 0; i < CPS1_ROMSET_QSOUND_CHIPS; i++) {
        qs[i] = find_crc(crcs, count, set->qsound_crc[i]);
        if (qs[i] < 0)
            return 0;           /* all-or-nothing: half a bank plays garbage */
    }

    *audio_index = aud;
    for (unsigned i = 0; i < CPS1_ROMSET_QSOUND_CHIPS; i++)
        qsound_index[i] = qs[i];
    return 1;
}

const cps1_romset_t *cps1_romset_match(const uint32_t *crcs, unsigned count,
                                        int prg_index[CPS1_ROMSET_PRG_CHIPS],
                                        int gfx_index[CPS1_ROMSET_GFX_CHIPS])
{
    if (crcs == NULL || prg_index == NULL || gfx_index == NULL)
        return NULL;

    for (unsigned s = 0; s < cps1_romset_count; s++) {
        const cps1_romset_t *set = &cps1_romsets[s];
        if (cps1_romset_resolve(set, crcs, count, prg_index, gfx_index) == 0)
            return set;
    }
    return NULL;
}

static unsigned count_missing(const cps1_romset_t *set, const uint32_t *crcs, unsigned count)
{
    unsigned missing = 0;
    for (unsigned i = 0; i < CPS1_ROMSET_PRG_CHIPS; i++)
        if (find_crc(crcs, count, set->prg_crc[i]) < 0)
            missing++;
    for (unsigned i = 0; i < CPS1_ROMSET_GFX_CHIPS; i++)
        if (find_crc(crcs, count, set->gfx_crc[i]) < 0)
            missing++;
    return missing;
}

const cps1_romset_t *cps1_romset_closest(const uint32_t *crcs, unsigned count,
                                          unsigned *missing_out)
{
    const cps1_romset_t *best = NULL;
    unsigned best_missing = 0;

    for (unsigned s = 0; s < cps1_romset_count; s++) {
        unsigned missing = count_missing(&cps1_romsets[s], crcs, count);
        if (best == NULL || missing < best_missing) {
            best = &cps1_romsets[s];
            best_missing = missing;
        }
    }
    if (missing_out != NULL)
        *missing_out = best_missing;
    return best;
}
'''


def render(data):
    chip_size = data["chip_size"]
    out = [HEADER]
    for rs in data["romsets"]:
        prg = rs["prg"]
        gfx = rs["gfx"]
        if len(prg) != 2:
            sys.exit(f"{rs['name']}: expected 2 program chips, got {len(prg)}")
        if len(gfx) != 8:
            sys.exit(f"{rs['name']}: expected 8 graphics chips, got {len(gfx)}")

        # Sound is OPTIONAL, per romset: a set may legitimately have no sound
        # chips listed yet. All-or-nothing within a set, though -- a partial
        # QSound bank would resolve and then play garbage.
        aud = rs.get("audiocpu", [])
        qs = rs.get("qsound", [])
        if len(aud) not in (0, 1):
            sys.exit(f"{rs['name']}: expected 0 or 1 audio CPU chip, got {len(aud)}")
        if len(qs) not in (0, 4):
            sys.exit(f"{rs['name']}: expected 0 or 4 QSound chips, got {len(qs)}")
        if bool(aud) != bool(qs):
            sys.exit(f"{rs['name']}: audiocpu and qsound must both be present or both absent")

        note = rs.get("_note")
        out.append(f"    /* {rs['title']}")
        if rs.get("parent"):
            out.append(f"     * MAME clone of '{rs['parent']}'.")
        if note:
            # keep the note readable at 78 columns
            words, line = note.split(), "     *"
            for w in words:
                if len(line) + 1 + len(w) > 78:
                    out.append(line)
                    line = "     *"
                line += " " + w
            out.append(line)
        out.append("     */")
        out.append("    {")
        out.append(f'        .name = "{rs["name"]}",')
        out.append("        .prg_crc = { " + ", ".join(f'0x{c["crc32"]}u' for c in prg) + " },")
        out.append("        .gfx_crc = { " +
                   ", ".join(f'0x{c["crc32"]}u' for c in gfx[:4]) + ",")
        out.append("                     " +
                   ", ".join(f'0x{c["crc32"]}u' for c in gfx[4:]) + " },")
        out.append("        .audio_crc = " +
                   (f'0x{aud[0]["crc32"]}u,' if aud else "0u,"))
        out.append("        .qsound_crc = { " +
                   (", ".join(f'0x{c["crc32"]}u' for c in qs) if qs
                    else "0u, 0u, 0u, 0u") + " },")
        out.append("    },")
    out.append(FOOTER)
    text = "\n".join(out)
    # The chip size is a header constant; assert the JSON agrees rather than
    # emitting a second definition that could disagree with it.
    if chip_size != 0x80000:
        sys.exit(f"chip_size {chip_size} is not the 512 KB CPS1_ROMSET_CHIP_SIZE assumes")
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if the generated file is stale")
    args = ap.parse_args()

    data = json.loads(JSON_PATH.read_text())
    text = render(data)

    if args.check:
        current = C_PATH.read_text() if C_PATH.exists() else ""
        if current != text:
            print(f"STALE: {C_PATH.relative_to(ROOT)} does not match "
                  f"{JSON_PATH.relative_to(ROOT)}", file=sys.stderr)
            print("       regenerate with: python3 tools/gen_cps1_romset.py", file=sys.stderr)
            return 1
        print(f"ok: {C_PATH.relative_to(ROOT)} matches {JSON_PATH.relative_to(ROOT)}")
        return 0

    C_PATH.write_text(text)
    print(f"wrote {C_PATH.relative_to(ROOT)} "
          f"({len(data['romsets'])} romsets from {JSON_PATH.relative_to(ROOT)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
