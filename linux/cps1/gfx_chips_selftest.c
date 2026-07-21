/*
 * Proves the device's graphics path: un-assembled MAME chips, read in place.
 *
 * On the device there is no 4 MB assembled GFX region -- RAM_EMU is 724 KB, so
 * the eight chip dumps stay where odroid_overlay_cache_file_in_flash() put
 * them and cps1_gfx_chip_byte() does MAME's interleave as address arithmetic.
 * Two things can go wrong there and neither one announces itself:
 *
 *   1. the interleave formula, and
 *   2. WHICH FILE GOES IN WHICH SLOT -- tk2_gfx3.rom belongs in slot 1 and
 *      tk2_gfx2.rom in slot 2, and in the Japanese set the upper four chips
 *      are named tk205..tk208 and sort ahead of tk2_gfx1..4 entirely. Fill the
 *      slots in filename order and every file loads, every size checks out,
 *      and two bitplane pairs are quietly cross-wired.
 *
 * So this does not compare the chip path against itself. The reference is the
 * assembled .cps1 image built by a SEPARATE program (tools/cps1_rom_pack.py)
 * from MAME's own ROM_START table, which is the image the renderer has already
 * been shown to draw a correct title screen from. Agreement between the two is
 * evidence; agreement of the formula with itself would not be.
 *
 * Both stages must pass:
 *   A. every byte of the 4 MB region, gathered from chips == assembled image
 *   B. tiles decoded through the real decoders (which is all the renderer ever
 *      touches) are identical either way, for all three layer geometries
 *
 * Usage: cps1-gfx-chips-selftest <reference.cps1> <romset_folder>
 * Skips (exit 0, says so) when either is absent -- a safety net that fails the
 * build on a machine with no ROMs teaches people to ignore the build.
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cps1_rom.h"
#include "cps1_romset.h"

#define GFX_REGION_SIZE 0x400000u

static uint8_t *read_whole_file(const char *path, uint32_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *size_out = (uint32_t)n;
    return buf;
}

/* The .cps1 container's header, only as far as the gfx region's offset/size.
 * The format is retired; it survives here purely as an independently-built
 * reference image. */
typedef struct { char m[4]; uint16_t ver, flags; char set[16];
                 uint32_t po, ps, go, gs; } hdr_prefix_t;

int main(int argc, char **argv)
{
    /* Defaults so a bare run in the suite skips politely rather than failing
     * the build on a machine with no ROMs -- a safety net that breaks the
     * build for the wrong reason teaches people to ignore it. */
    const char *ref_path = (argc > 1) ? argv[1] : "/tmp/cps1_rom/wofj.cps1";
    const char *dir_path = (argc > 2) ? argv[2] : "/tmp/cps1_folder/wofj";
    /* Optional shared pool, named by CRC32 -- see main_cps1.c. */
    const char *shared_path = (argc > 3) ? argv[3] : NULL;

    uint32_t ref_size = 0;
    uint8_t *ref = read_whole_file(ref_path, &ref_size);
    if (!ref) {
        printf("[gfxchips] SKIP: no reference image at %s\n", ref_path);
        return 0;
    }
    hdr_prefix_t h;
    memcpy(&h, ref, sizeof(h));
    if (memcmp(h.m, "CPS1", 4) != 0 || h.go + h.gs > ref_size || h.gs != GFX_REGION_SIZE) {
        printf("[gfxchips] SKIP: %s is not a usable reference image\n", ref_path);
        free(ref);
        return 0;
    }
    const uint8_t *assembled = ref + h.go;

    /* --- the folder loader's identification pass, host side ---
     * Same rule as main_cps1.c: consider only files of exactly one chip size,
     * hash the contents, let cps1_romset_match() assign the slots. */
    DIR *d = opendir(dir_path);
    if (!d) {
        printf("[gfxchips] SKIP: no romset folder at %s\n", dir_path);
        free(ref);
        return 0;
    }
    static uint32_t crcs[16];
    static uint8_t *datas[16];
    unsigned count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < 16) {
        if (e->d_name[0] == '.')
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir_path, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if ((uint32_t)st.st_size != CPS1_ROMSET_CHIP_SIZE)
            continue;
        uint32_t sz = 0;
        uint8_t *buf = read_whole_file(path, &sz);
        if (!buf)
            continue;
        datas[count] = buf;
        crcs[count] = cps1_crc32(buf, sz);
        count++;
    }
    closedir(d);

    int prg_index[CPS1_ROMSET_PRG_CHIPS], gfx_index[CPS1_ROMSET_GFX_CHIPS];
    const cps1_romset_t *set = cps1_romset_match(crcs, count, prg_index, gfx_index);

    /* Second phase, mirroring main_cps1.c: a clone folder is incomplete on its
     * own, so fetch precisely the chips the closest set is missing from the
     * shared pool, addressed BY HASH. Nothing else in the pool is opened. */
    if (!set && shared_path) {
        unsigned missing = 0;
        const cps1_romset_t *near = cps1_romset_closest(crcs, count, &missing);
        unsigned added = 0;
        if (near && missing) {
            uint32_t want[CPS1_ROMSET_PRG_CHIPS + CPS1_ROMSET_GFX_CHIPS];
            unsigned nw = 0;
            for (unsigned i = 0; i < CPS1_ROMSET_PRG_CHIPS; i++) want[nw++] = near->prg_crc[i];
            for (unsigned i = 0; i < CPS1_ROMSET_GFX_CHIPS; i++) want[nw++] = near->gfx_crc[i];
            for (unsigned w = 0; w < nw && count < 16; w++) {
                int have = 0;
                for (unsigned i = 0; i < count; i++) if (crcs[i] == want[w]) have = 1;
                if (have) continue;
                char path[1024];
                snprintf(path, sizeof(path), "%s/%08x.bin", shared_path, want[w]);
                uint32_t sz = 0;
                uint8_t *buf = read_whole_file(path, &sz);
                if (!buf) continue;
                uint32_t got = cps1_crc32(buf, sz);
                if (got != want[w]) {           /* the name is checked, not trusted */
                    printf("[gfxchips] FAIL: %s hashes to %08X\n", path, got);
                    return 1;
                }
                datas[count] = buf;
                crcs[count] = got;
                count++;
                added++;
            }
        }
        printf("[gfxchips] shared pool supplied %u of %u missing chips for '%s'\n",
               added, missing, near ? near->name : "?");
        set = cps1_romset_match(crcs, count, prg_index, gfx_index);
    }

    if (!set) {
        printf("[gfxchips] FAIL: no complete romset in %s (%u candidate chips)\n",
               dir_path, count);
        return 1;
    }
    printf("[gfxchips] romset '%s' from %u chips; gfx slots =", set->name, count);
    for (unsigned i = 0; i < CPS1_ROMSET_GFX_CHIPS; i++)
        printf(" %d", gfx_index[i]);
    printf("\n");

    cps1_gfx_chips_t chips;
    memset(&chips, 0, sizeof(chips));
    for (unsigned i = 0; i < CPS1_ROMSET_GFX_CHIPS; i++)
        chips.chip[i] = datas[gfx_index[i]];
    chips.chip_size = CPS1_ROMSET_CHIP_SIZE;
    chips.chip_count = CPS1_ROMSET_GFX_CHIPS;

    cps1_rom_t rom_chips, rom_flat;
    memset(&rom_chips, 0, sizeof(rom_chips));
    memset(&rom_flat, 0, sizeof(rom_flat));
    cps1_rom_region_t prg = { datas[prg_index[0]], CPS1_ROMSET_CHIP_SIZE };
    if (cps1_rom_attach_chips(&rom_chips, prg, &chips) != 0) {
        printf("[gfxchips] FAIL: attach_chips rejected a complete set\n");
        return 1;
    }
    cps1_rom_region_t gfx_flat = { assembled, GFX_REGION_SIZE };
    cps1_rom_region_t none = { NULL, 0 };
    if (cps1_rom_attach(&rom_flat, prg, gfx_flat, none, none) != 0) {
        printf("[gfxchips] FAIL: attach rejected the reference image\n");
        return 1;
    }

    if (cps1_rom_gfx_size(&rom_chips) != GFX_REGION_SIZE) {
        printf("[gfxchips] FAIL: chip set spans %u bytes, expected %u\n",
               cps1_rom_gfx_size(&rom_chips), GFX_REGION_SIZE);
        return 1;
    }

    /* --- A. every byte of the region --- */
    unsigned long mismatches = 0;
    uint32_t first_bad = 0;
    for (uint32_t off = 0; off < GFX_REGION_SIZE; off++) {
        if (cps1_rom_gfx_byte(&rom_chips, off) != assembled[off]) {
            if (mismatches == 0)
                first_bad = off;
            mismatches++;
        }
    }
    if (mismatches) {
        printf("[gfxchips] FAIL: %lu/%u bytes differ, first at 0x%06X\n",
               mismatches, GFX_REGION_SIZE, first_bad);
        return 1;
    }
    printf("[gfxchips] A: all %u region bytes agree with the reference image\n",
           GFX_REGION_SIZE);

    /* --- B. through the decoders the renderer actually calls ---
     * sub = 1/2/4 is SCROLL1 / SCROLL2+sprites / SCROLL3, i.e. all three
     * geometries, and every quadrant of each. */
    unsigned long tiles = 0, tile_bad = 0;
    for (unsigned sub = 1; sub <= 4; sub <<= 1) {
        uint32_t tile_bytes = sub * sub * 32u;
        uint32_t max_code = GFX_REGION_SIZE / tile_bytes;
        for (uint32_t code = 0; code < max_code; code += 97) {   /* prime stride */
            for (unsigned qy = 0; qy < sub; qy++) {
                for (unsigned qx = 0; qx < sub; qx++) {
                    uint8_t a[CPS1_TILE_SIZE_BYTES], b[CPS1_TILE_SIZE_BYTES];
                    int ra = cps1_rom_decode_subtile(&rom_chips, sub, code, qx, qy, a);
                    int rb = cps1_rom_decode_subtile(&rom_flat, sub, code, qx, qy, b);
                    tiles++;
                    if (ra != rb || (ra == 0 && memcmp(a, b, sizeof(a)) != 0)) {
                        if (tile_bad == 0)
                            printf("[gfxchips] first bad tile: sub=%u code=%u q=(%u,%u) "
                                   "rc %d/%d\n", sub, code, qx, qy, ra, rb);
                        tile_bad++;
                    }
                }
            }
        }
    }
    /* The 8x8 planar path (real_gfx tile cache) reads bit-by-bit rather than
     * in 4-byte groups, so it gets its own sweep. */
    for (uint32_t idx = 0; idx < GFX_REGION_SIZE / 64u; idx += 397) {
        uint8_t a[CPS1_TILE_SIZE_BYTES], b[CPS1_TILE_SIZE_BYTES];
        int ra = cps1_rom_decode_tile_planar(&rom_chips, &CPS1_GFX_LAYOUT_8X8_LEFT, idx, a);
        int rb = cps1_rom_decode_tile_planar(&rom_flat, &CPS1_GFX_LAYOUT_8X8_LEFT, idx, b);
        tiles++;
        if (ra != rb || (ra == 0 && memcmp(a, b, sizeof(a)) != 0))
            tile_bad++;
    }
    if (tile_bad) {
        printf("[gfxchips] FAIL: %lu/%lu decoded tiles differ\n", tile_bad, tiles);
        return 1;
    }
    printf("[gfxchips] B: all %lu decoded tiles identical (8x8 / 16x16 / 32x32)\n", tiles);

    printf("[gfxchips] PASS\n");
    for (unsigned i = 0; i < count; i++)
        free(datas[i]);
    free(ref);
    return 0;
}
