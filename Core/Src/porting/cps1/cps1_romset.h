#pragma once
/*
 * Which chip file goes in which slot -- by CRC32 of its contents, never by
 * its name.
 *
 * A MAME romset is a folder of chip dumps whose ROM_LOAD order is NOT their
 * filename order, and CPS-1 is a live example rather than a hypothetical:
 *
 *     tk2_gfx3.rom (CRC 45227027) -> gfx slot 1
 *     tk2_gfx2.rom (CRC c5ca2460) -> gfx slot 2
 *
 * and in the Japanese set the four upper-half chips are named tk205..tk208,
 * which sort BEFORE tk2_gfx1..4 entirely. Fill the slots in filename order and
 * every load succeeds, every size checks out, and two bitplane pairs are
 * silently cross-wired -- a corrupt picture with nothing anywhere reporting an
 * error. MAME's own ROM_LOAD macros bind a chip's *hash* to an offset for
 * exactly this reason, and so does this table.
 *
 * The CRCs below are transcribed from mamedev/mame src/mame/capcom/cps1.cpp's
 * ROM_START(wof)/ROM_START(wofr1)/ROM_START(wofj) blocks. The slot ORDER was
 * independently re-derived from real dumps: each chip file was matched against
 * the assembled 4 MB GFX image the renderer already renders correctly, and the
 * eight slot assignments came back exactly as the table says. It is data, not
 * a guess -- but it is also data this repo cannot regenerate on its own, so
 * treat a new romset as needing its MAME entry read, not inferred.
 */
#include <stdint.h>

#define CPS1_ROMSET_GFX_CHIPS 8
#define CPS1_ROMSET_PRG_CHIPS 2

/* Every CPS-1 chip in the sets below is 512 KB. A file of any other size
 * cannot be one, which is what lets the loader skip the PAL dumps (279 B
 * each) without reading them. */
#define CPS1_ROMSET_CHIP_SIZE 0x80000u

typedef struct {
    const char *name;
    /* Program ROM chips in ascending 68000 address order: prg[0] holds
     * 0x000000-0x07FFFF (and therefore the reset vector), prg[1] holds
     * 0x080000-0x0FFFFF. */
    uint32_t prg_crc[CPS1_ROMSET_PRG_CHIPS];
    /* GFX chips in cps1_gfx_chip_byte() slot order -- slots 0-3 are the
     * 0x000000 half, slots 4-7 the 0x200000 half. */
    uint32_t gfx_crc[CPS1_ROMSET_GFX_CHIPS];
} cps1_romset_t;

extern const cps1_romset_t cps1_romsets[];
extern const unsigned cps1_romset_count;

/* Standard (zlib/MAME) CRC32 of a buffer: seeded with ~0 and inverted at the
 * end. This is NOT what the firmware's own crc32_le() returns on its own --
 * that one is the raw table update with neither inversion, so feeding it a 0
 * seed produces a number that will never match a MAME hash. */
uint32_t cps1_crc32(const uint8_t *data, uint32_t len);

/*
 * Given `count` chip CRCs gathered from a game folder (in whatever order the
 * directory happened to list them), find the first romset every one of whose
 * required chips is present, and write each chip's index within `crcs` to the
 * matching slot.
 *
 * Returns the romset, or NULL if no set is complete -- which is the case that
 * matters most: a MAME "split set" clone archive holds only the chips unique
 * to it (wofj.zip carries its 2 program chips and the 4 upper GFX chips and
 * nothing else), so a folder made by extracting one alone is missing half its
 * graphics. Reporting that beats booting a game with four blank bitplanes.
 */
const cps1_romset_t *cps1_romset_match(const uint32_t *crcs, unsigned count,
                                        int prg_index[CPS1_ROMSET_PRG_CHIPS],
                                        int gfx_index[CPS1_ROMSET_GFX_CHIPS]);

/*
 * Same question asked about ONE named set instead of "whichever matches
 * first". Returns 0 and fills the index slots when every chip of `set` is
 * present in `crcs`, -1 otherwise, and writes nothing on failure.
 *
 * cps1_romset_match() cannot answer this. When a game folder pools several
 * archives -- a clone beside the parent it borrows from -- MORE THAN ONE set
 * can be complete out of the same chips, and "first in the table" is not a
 * choice anybody made. The caller enumerates the runnable sets with this and
 * then either runs the only one or asks the player which.
 */
int cps1_romset_resolve(const cps1_romset_t *set, const uint32_t *crcs, unsigned count,
                         int prg_index[CPS1_ROMSET_PRG_CHIPS],
                         int gfx_index[CPS1_ROMSET_GFX_CHIPS]);

/*
 * When nothing matched, say something useful instead of "not found": report
 * the set the folder came CLOSEST to and how many of its chips are absent.
 * With a clone archive extracted on its own that reads "wofj: 4 of 10 chips
 * missing", which points straight at the parent set -- as opposed to a bare
 * failure, which looks identical to an unsupported game.
 *
 * Returns NULL only when there are no romsets at all. `missing_out` may be
 * NULL. A return with *missing_out == 0 means the set is in fact complete.
 */
const cps1_romset_t *cps1_romset_closest(const uint32_t *crcs, unsigned count,
                                          unsigned *missing_out);
