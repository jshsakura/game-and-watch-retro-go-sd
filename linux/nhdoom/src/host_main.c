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

    Z_Init();
    InitGlobals();
    D_DoomMain();   /* never returns; host_video exits on frame limit */
    return 0;
}
