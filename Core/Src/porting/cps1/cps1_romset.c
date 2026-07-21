/* CRC32-keyed chip -> slot tables. See cps1_romset.h for why not filenames. */
#include <stddef.h>

#include "cps1_romset.h"

/* The firmware's shared table-driven CRC32 update (Core/Src/porting/crc32.c).
 * Declared here rather than pulled in via a header because the only header
 * that declares it lives in the host build tree. */
extern unsigned int crc32_le(unsigned int crc, unsigned char const *buf, unsigned int len);

uint32_t cps1_crc32(const uint8_t *data, uint32_t len)
{
    /* crc32_le() already inverts on the way in and on the way out, so a 0 seed
     * gives the standard zlib/MAME CRC32 with nothing added. Wrapping it in
     * another pair of inversions -- which its name invites -- cancels the
     * inner ones and produces a number that matches no MAME hash. */
    return crc32_le(0, (unsigned char const *)data, len);
}

const cps1_romset_t cps1_romsets[] = {
    /* ROM_START( wofj ) -- Tenchi wo Kurau II: Sekiheki no Tatakai (Japan
     * 921031). Listed first because it is the set this port was brought up
     * on. Its program chips and its four UPPER GFX chips are unique to it;
     * the four lower GFX chips are byte-identical to the parent's, so a
     * working folder needs the parent archive's chips alongside it. */
    {
        .name = "wofj",
        .prg_crc = { 0x9b215a68u, 0xb74b09acu },        /* tk2j_23c, tk2j_22c */
        .gfx_crc = { 0x0d9cb9bfu, 0x45227027u, 0xc5ca2460u, 0xe349551cu,
                     0xe4a44d53u, 0x58066ba8u, 0xd706568eu, 0xd4a19a02u },
    },
    /* ROM_START( wofr1 ) -- Warriors of Fate (World 921002). This is what the
     * archive labelled "wof.zip" actually contains: its program CRCs are
     * wofr1's, not wof's. The zip's name does not describe its contents,
     * which is one more reason the loader keys on hashes. */
    {
        .name = "wofr1",
        .prg_crc = { 0x11fb2ed1u, 0x479b3f24u },        /* tk2e_23b, tk2e_22b */
        .gfx_crc = { 0x0d9cb9bfu, 0x45227027u, 0xc5ca2460u, 0xe349551cu,
                     0x291f0f0bu, 0x3edeb949u, 0x1abd14d6u, 0xb27948e3u },
    },
    /* ROM_START( wof ) -- Warriors of Fate (World 921031). Same GFX as wofr1,
     * a later program revision. No dump of it has been seen by this port; the
     * entry costs 40 bytes and means a user who has one is not told their
     * complete romset is unrecognised. */
    {
        .name = "wof",
        .prg_crc = { 0x0d708505u, 0x608c17e3u },        /* tk2e_23c, tk2e_22c */
        .gfx_crc = { 0x0d9cb9bfu, 0x45227027u, 0xc5ca2460u, 0xe349551cu,
                     0x291f0f0bu, 0x3edeb949u, 0x1abd14d6u, 0xb27948e3u },
    },
};

const unsigned cps1_romset_count = sizeof(cps1_romsets) / sizeof(cps1_romsets[0]);

/* How many of `set`'s chips are absent from the pool. */
static unsigned count_missing(const cps1_romset_t *set, const uint32_t *crcs, unsigned count);

static int find_crc(const uint32_t *crcs, unsigned count, uint32_t want)
{
    for (unsigned i = 0; i < count; i++)
        if (crcs[i] == want)
            return (int)i;
    return -1;
}

const cps1_romset_t *cps1_romset_match(const uint32_t *crcs, unsigned count,
                                        int prg_index[CPS1_ROMSET_PRG_CHIPS],
                                        int gfx_index[CPS1_ROMSET_GFX_CHIPS])
{
    if (crcs == NULL || prg_index == NULL || gfx_index == NULL)
        return NULL;

    for (unsigned s = 0; s < cps1_romset_count; s++) {
        const cps1_romset_t *set = &cps1_romsets[s];
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
            continue;

        for (unsigned i = 0; i < CPS1_ROMSET_PRG_CHIPS; i++)
            prg_index[i] = prg[i];
        for (unsigned i = 0; i < CPS1_ROMSET_GFX_CHIPS; i++)
            gfx_index[i] = gfx[i];
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
