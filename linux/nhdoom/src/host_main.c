/* nhdoom host port: headless entry point.
 *
 * Recreates the essential startup of src/main.c for a Linux/arm32 host:
 *   1. mmap the converted WAD read-only (XIP external flash),
 *   2. mmap a writable, 0xFF-initialised region to back the w_wad.c internal-
 *      flash cache (programFlashWord only writes blank 0xFFFFFFFF words),
 *   3. publish the three pointer-packing bases the shadow i_memory.h reads:
 *        - nh_ram_ptr_base   = linker-pinned 256KB engine-data window,
 *        - nh_ext_flash_base = WAD mapping,
 *        - nh_flash_addr_base= flash-cache mapping,
 *   4. start the QSPI-DMA emulation thread,
 *   5. Z_Init / InitGlobals / D_DoomMain (auto-warps to a level via the
 *      DEBUG_SETUP/AUTOSTART path in d_main.c).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "i_memory.h"
#include "z_zone.h"
#include "global_data.h"
#include "nhdoom/nhdoom_flashcache.h"

/* runtime pointer-packing bases consumed by the shadow i_memory.h. */
unsigned long nh_ram_ptr_base;
unsigned long nh_ext_flash_base;
unsigned long nh_ext_flash_size;
unsigned long nh_flash_addr_base;

extern char nh_ram_window_base[];        /* linker symbol (256KB-aligned) */
extern unsigned char staticZone[];       /* z_zone.c, lives in that window */
/* WAD access pointers — normally in doom_iwad.c, whose static init derives from
 * the (now runtime) WAD_ADDRESS and can't be a constant. We own them here and
 * set them once the WAD is mapped; doom_iwad.c is excluded from the build. */
const unsigned char *doom_iwad = 0;
const unsigned int  *p_doom_iwad_len = 0;
static unsigned int nh_wad_len;
extern void nh_start_qspi_dma(void);
extern void initGraphics(void);
extern void Z_Init(void);
extern void InitGlobals(void);
extern void D_DoomMain(void);

#define DEFAULT_WAD \
  "/tmp/claude-1001/-home-ubuntu-app-jupyterLab-notebooks-game-and-watch-retro-go-sd/" \
  "7385a58b-d718-442b-9d6c-2950a87416bc/scratchpad/p0/doom1.mcu.wad"

static size_t page_up(size_t v) { size_t p = 4096; return (v + p - 1) & ~(p - 1); }

static void map_wad(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open wad"); exit(2); }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat wad"); exit(2); }
    void *p = mmap(NULL, page_up(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) { perror("mmap wad"); exit(2); }
    close(fd);
    /* The .mcu.wad is a raw WAD (IWAD magic at offset 0; no length prefix).
     * The engine's WAD_ADDRESS == EXT_FLASH_BASE + 4 must point at the IWAD, so
     * base the external-flash region 4 bytes "before" the mapping. p_doom_iwad_len
     * (= WAD_ADDRESS - 4) would land on the unmapped pre-byte, so it is redirected
     * to a real length word here instead. */
    nh_ext_flash_base = (unsigned long)(uintptr_t)p - 4;
    nh_ext_flash_size = (unsigned long)st.st_size + 4;
    nh_wad_len = (unsigned int)st.st_size;
    doom_iwad = (const unsigned char *)p;          /* WAD_ADDRESS == IWAD */
    p_doom_iwad_len = &nh_wad_len;
    fprintf(stderr, "[nhdoom] WAD mmap'd @%p size=%ld magic=%c%c%c%c\n",
            p, (long)st.st_size,
            ((char*)p)[0], ((char*)p)[1], ((char*)p)[2], ((char*)p)[3]);
}

static void map_flash_cache(void)
{
    size_t sz = page_up(FLASH_CACHE_REGION_SIZE);
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap flash-cache"); exit(2); }
    memset(p, 0xFF, sz);   /* erased-flash state, required by programFlashWord */
    nh_flash_addr_base = (unsigned long)(uintptr_t)p;
    fprintf(stderr, "[nhdoom] flash-cache mmap'd @%p size=%zu (0xFF)\n", p, sz);
}

/* ---- pre-baked flash-cache: BAKE (dump) and VERIFY (preload+relocate) ---- *
 * BAKE  (NHDOOM_BAKE=<out>): after a normal run has populated the cache, scan
 *   it, classify every non-0xFF word as data / WAD-pointer / cache-pointer, and
 *   write doom1.flashcache.bin (header + cache data + reloc table). Called from
 *   the host_video.c exit path.
 * VERIFY (NHDOOM_VERIFY=<in>): before Z_Init, copy a previously baked cache into
 *   the (freshly mmap'd, possibly differently-based) cache region and relocate
 *   its pointers to THIS run's WAD/cache bases. The engine then recomputes and
 *   every write no-ops (storeWordToFlash) / matches (updateLumpAddresses, which
 *   while(1)-hangs on a mismatch) — so a clean run + identical frames proves the
 *   relocation is correct, exactly mirroring the device load. */
void nhdoom_bake_dump(void)
{
    const char *out = getenv("NHDOOM_BAKE");
    if (!out) return;

    const uint32_t *fc = (const uint32_t *)(uintptr_t)nh_flash_addr_base;
    unsigned long nwords = (unsigned long)FLASH_CACHE_REGION_SIZE / 4;
    uint32_t wadlo = (uint32_t)nh_ext_flash_base;
    uint32_t wadhi = (uint32_t)(nh_ext_flash_base + nh_ext_flash_size);
    uint32_t fclo  = (uint32_t)nh_flash_addr_base;
    uint32_t fchi  = (uint32_t)(nh_flash_addr_base + FLASH_CACHE_REGION_SIZE);

    unsigned long hi = 0;
    for (unsigned long i = 0; i < nwords; i++) if (fc[i] != 0xFFFFFFFFu) hi = i;
    uint32_t used = (uint32_t)((hi + 1) * 4);
    uint32_t cache_bytes = (used + NHDOOM_FC_PAGE - 1) & ~(NHDOOM_FC_PAGE - 1);

    /* static (not malloc): the QSPI emulation pthread spins and a malloc here
     * can contend the libc arena lock and stall. host-only, so a fixed buffer is
     * fine. */
    static uint32_t reloc[FLASH_CACHE_REGION_SIZE / 4];
    uint32_t nr = 0;
    for (unsigned long i = 0; i <= hi; i++) {
        uint32_t w = fc[i];
        if (w == 0xFFFFFFFFu) continue;
        if (w >= wadlo && w < wadhi)      reloc[nr++] = (uint32_t)(i * 4) | NHDOOM_FC_RELOC_WAD;
        else if (w >= fclo && w < fchi)   reloc[nr++] = (uint32_t)(i * 4) | NHDOOM_FC_RELOC_CACHE;
    }

    nhdoom_fc_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic          = NHDOOM_FC_MAGIC;
    h.version        = NHDOOM_FC_VERSION;
    h.cache_bytes    = cache_bytes;
    h.host_wad_base  = (uint32_t)nh_ext_flash_base;
    h.host_wad_size  = (uint32_t)nh_ext_flash_size;
    h.host_cache_base= (uint32_t)nh_flash_addr_base;
    h.host_cache_size= (uint32_t)FLASH_CACHE_REGION_SIZE;
    h.n_reloc        = nr;
    h.data_offset    = NHDOOM_FC_PAGE;                 /* cache data on a sector */
    h.reloc_offset   = NHDOOM_FC_PAGE + cache_bytes;   /* reloc on its own sector */

    FILE *f = fopen(out, "wb");
    if (!f) { perror("open bake out"); return; }
    uint8_t pad[NHDOOM_FC_PAGE];
    memset(pad, 0xFF, sizeof(pad));
    fwrite(&h, sizeof(h), 1, f);
    fwrite(pad, 1, NHDOOM_FC_PAGE - sizeof(h), f);     /* pad header -> sector */
    fwrite(fc, 1, used, f);
    fwrite(pad, 1, cache_bytes - used, f);             /* pad cache -> sector */
    fwrite(reloc, sizeof(uint32_t), nr, f);
    fclose(f);
    fprintf(stderr, "[BAKE] wrote %s: cache=%uB (used=%uB) reloc=%u "
            "(wad/cache ptrs) wad_base=0x%x cache_base=0x%x\n",
            out, cache_bytes, used, nr,
            h.host_wad_base, h.host_cache_base);
}

void nhdoom_verify_preload(void)
{
    const char *in = getenv("NHDOOM_VERIFY");
    if (!in) return;

    FILE *f = fopen(in, "rb");
    if (!f) { perror("open verify in"); exit(2); }
    nhdoom_fc_header_t h;
    if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != NHDOOM_FC_MAGIC) {
        fprintf(stderr, "[VERIFY] bad flashcache file\n"); exit(2);
    }

    uint8_t *cache = (uint8_t *)(uintptr_t)nh_flash_addr_base;
    fseek(f, h.data_offset, SEEK_SET);
    if (fread(cache, 1, h.cache_bytes, f) != h.cache_bytes) {
        fprintf(stderr, "[VERIFY] short cache read\n"); exit(2);
    }
    /* static, not malloc: avoid libc-arena-lock contention with the spinning
     * QSPI pthread (already started here), which can deadlock. */
    static uint32_t reloc[FLASH_CACHE_REGION_SIZE / 4];
    if (h.n_reloc > FLASH_CACHE_REGION_SIZE / 4) {
        fprintf(stderr, "[VERIFY] n_reloc too large\n"); exit(2);
    }
    fseek(f, h.reloc_offset, SEEK_SET);
    if (fread(reloc, sizeof(uint32_t), h.n_reloc, f) != h.n_reloc) {
        fprintf(stderr, "[VERIFY] short reloc read\n"); exit(2);
    }
    fclose(f);

    int32_t wad_delta   = (int32_t)((uint32_t)nh_ext_flash_base  - h.host_wad_base);
    int32_t cache_delta = (int32_t)((uint32_t)nh_flash_addr_base - h.host_cache_base);
    uint32_t whlo = h.host_wad_base,   whhi = h.host_wad_base   + h.host_wad_size;
    uint32_t chlo = h.host_cache_base, chhi = h.host_cache_base + h.host_cache_size;
    uint32_t done = 0;
    for (uint32_t k = 0; k < h.n_reloc; k++) {
        uint32_t off  = reloc[k] & NHDOOM_FC_OFFSET_MASK;
        uint32_t type = reloc[k] & NHDOOM_FC_TYPE_MASK;
        uint32_t *w = (uint32_t *)(cache + off);
        if (type == NHDOOM_FC_RELOC_CACHE) {
            if (*w >= chlo && *w < chhi) { *w = (uint32_t)((int32_t)*w + cache_delta); done++; }
        } else {
            if (*w >= whlo && *w < whhi) { *w = (uint32_t)((int32_t)*w + wad_delta); done++; }
        }
    }
    fprintf(stderr, "[VERIFY] preloaded %uB, relocated %u/%u words "
            "(wad_delta=%d cache_delta=%d)\n",
            h.cache_bytes, done, h.n_reloc, wad_delta, cache_delta);
}

/* Called by w_wad.c (NHDOOM_BAKE_HOOK) the instant the flash cache is fully built
 * -- BEFORE the first frame render, which the host engine crashes ~50% of the
 * time due to a pre-existing layout/demo-sensitive packed-pointer bug. Acting
 * here makes BAKE and the write-count VERIFY reliable. */
void nhdoom_cache_ready(void)
{
    static int once = 0;
    if (once++) return;                 /* first level's cache is enough */
#ifdef NHDOOM_COUNT_WRITES
    extern unsigned long nh_flash_write_count;
    fprintf(stderr, "[CACHE_READY] flash word writes during build = %lu\n",
            nh_flash_write_count);
#endif
    if (getenv("NHDOOM_BAKE")) {
        nhdoom_bake_dump();
        fprintf(stderr, "[CACHE_READY] baked, exiting before render\n");
        exit(0);
    }
    if (getenv("NHDOOM_VERIFY")) {
#ifdef NHDOOM_COUNT_WRITES
        fprintf(stderr, "[VERIFY-RESULT] writes=%lu (PASS iff 0)\n",
                nh_flash_write_count);
#endif
        if (!getenv("NHDOOM_VERIFY_RENDER")) exit(0);  /* reliable; skip render */
    }
    /* normal run (or NHDOOM_VERIFY_RENDER): fall through and render frames. */
}

int main(int argc, char **argv)
{
    const char *wad = getenv("NHDOOM_WAD");
    if (argc > 1) wad = argv[1];
    if (!wad) wad = DEFAULT_WAD;

    nh_ram_ptr_base = (unsigned long)(uintptr_t)nh_ram_window_base;

    map_wad(wad);
    map_flash_cache();

    fprintf(stderr, "[nhdoom] RAM_PTR_BASE=%#lx (window) staticZone=%p (off=%#lx) "
            "EXT_FLASH_BASE=%#lx FLASH_ADDRESS=%#lx\n",
            nh_ram_ptr_base, (void *)staticZone,
            (unsigned long)((uintptr_t)staticZone - nh_ram_ptr_base),
            nh_ext_flash_base, nh_flash_addr_base);
    if ((uintptr_t)staticZone < nh_ram_ptr_base ||
        (uintptr_t)staticZone >= nh_ram_ptr_base + RAM_WINDOW_SIZE)
        fprintf(stderr, "[nhdoom] WARNING: staticZone outside short-pointer window!\n");

    nh_start_qspi_dma();
    initGraphics();

    /* device-load simulation: if NHDOOM_VERIFY is set, preload a baked cache and
     * relocate it to these bases before the engine recomputes (no-op writes). */
    nhdoom_verify_preload();

    Z_Init();
    InitGlobals();
    D_DoomMain();   /* never returns; host_video exits on frame limit */
    return 0;
}
