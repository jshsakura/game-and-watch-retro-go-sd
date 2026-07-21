/*
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

    /* Tenchi wo Kurau II: Sekiheki no Tatakai (Japan 921031)
     * MAME clone of 'wof'.
     */
    {
        .name = "wofj",
        .prg_crc = { 0x9b215a68u, 0xb74b09acu },
        .gfx_crc = { 0x0d9cb9bfu, 0x45227027u, 0xc5ca2460u, 0xe349551cu,
                     0xe4a44d53u, 0x58066ba8u, 0xd706568eu, 0xd4a19a02u },
    },
    /* Warriors of Fate (World 921002)
     * MAME clone of 'wof'.
     * This is what the archive distributed as wof.zip actually contains: its
     * program CRCs are wofr1's, not wof's. A zip's name does not describe its
     * contents.
     */
    {
        .name = "wofr1",
        .prg_crc = { 0x11fb2ed1u, 0x479b3f24u },
        .gfx_crc = { 0x0d9cb9bfu, 0x45227027u, 0xc5ca2460u, 0xe349551cu,
                     0x291f0f0bu, 0x3edeb949u, 0x1abd14d6u, 0xb27948e3u },
    },
    /* Warriors of Fate (World 921031)
     * No dump of this set has been seen by this port. The entry costs 40
     * bytes and means a user who has one is not told their complete romset is
     * unrecognised.
     */
    {
        .name = "wof",
        .prg_crc = { 0x0d708505u, 0x608c17e3u },
        .gfx_crc = { 0x0d9cb9bfu, 0x45227027u, 0xc5ca2460u, 0xe349551cu,
                     0x291f0f0bu, 0x3edeb949u, 0x1abd14d6u, 0xb27948e3u },
    },
};

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
