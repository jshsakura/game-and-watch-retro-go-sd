/* host_main.c -- Linux host harness for the Game & Watch DOOM zone-allocation
 * failure. Mirrors the linux/<core> pattern: provides the platform glue
 * (the 6 doomgeneric DG_* callbacks) plus host stubs for the few G&W HAL
 * symbols the DOOM override files (Core/Src/porting/doom/*) reference, then
 * drives doomgeneric headless until DEMO1 loads E1M5 (or Z_Malloc fails).
 *
 * The whole point is fact-gathering: it walks the live DOOM zone (z_zone.c
 * mainzone) and reports the LARGEST contiguous reclaimable block vs the
 * requested allocation, so we can tell fragmentation from absolute shortfall.
 *
 * Knobs (env):
 *   DOOM_ZONE_KB    zone size in KiB        (default 490 -- matches device log)
 *   DOOM_BONUS_KB   bonus pool size in KiB  (default 150 -- matches device log)
 *   DOOM_MAX_TICKS  headless tick cap       (default 4000)
 *   DOOM_WAD        path to DOOM1.WAD
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "z_zone.h"   /* PU_FREE, PU_PURGELEVEL enum values (4 and 7) */

/* ---- knobs --------------------------------------------------------------- */
#define DEFAULT_ZONE_KB   490
#define DEFAULT_BONUS_KB  150
#define DEFAULT_MAX_TICKS 4000
#define DEFAULT_WAD \
  "/home/ubuntu/app/jupyterLab/notebooks/game-and-what/backend/data/library/public/roms/homebrew/DOOM1.WAD"

static int zone_kb;
static int bonus_kb;

/* ---- doomgeneric core entry points --------------------------------------- */
void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick(void);

/* doomstat: confirm which map the demo loaded (E1M5 == gamemap 5). */
extern int gamemap;

/* ========================================================================= */
/* Zone introspection. We mirror z_zone.c's private structs (identical types  */
/* => identical layout under the same compiler) and resolve `mainzone` by     */
/* name from the linker, so we can walk the live block list WITHOUT editing    */
/* z_zone.c.                                                                   */
/* ========================================================================= */
typedef struct hz_memblock_s {
    int                    size;   /* incl. header */
    void                 **user;
    int                    tag;    /* PU_FREE if free */
    int                    id;
    struct hz_memblock_s  *next;
    struct hz_memblock_s  *prev;
} hz_memblock_t;

typedef struct {
    int            size;
    hz_memblock_t  blocklist;
    hz_memblock_t *rover;
} hz_memzone_t;

extern hz_memzone_t *mainzone;   /* same symbol as z_zone.c's mainzone */

static int hz_is_reclaimable(const hz_memblock_t *b)
{
    return (b->tag == PU_FREE) || (b->tag >= PU_PURGELEVEL);
}

/* Largest contiguous run of reclaimable (free OR purgeable) blocks, in bytes,
 * INCLUDING block headers -- this is the size Z_Malloc's first-fit scan can
 * actually coalesce and hand out (it purges PU_PURGELEVEL+ on the way and
 * merges with adjacent free). Returns payload-usable bytes via *payload_out
 * (run minus one header). */
static int hz_largest_reclaimable(int *payload_out)
{
    if (!mainzone) { if (payload_out) *payload_out = 0; return 0; }

    int hdr = (int)sizeof(hz_memblock_t);
    int best_run = 0;
    int run = 0;
    hz_memblock_t *b;

    for (b = mainzone->blocklist.next; b != &mainzone->blocklist; b = b->next) {
        if (hz_is_reclaimable(b)) {
            run += b->size;
            if (run > best_run) best_run = run;
        } else {
            run = 0;
        }
    }
    if (payload_out) *payload_out = best_run > hdr ? best_run - hdr : 0;
    return best_run;
}

/* Total reclaimable bytes == Z_FreeMemory(). Also count blocks by class. */
static void hz_block_census(int *free_blocks, int *purge_blocks,
                            int *used_blocks, int tag_count[PU_NUM_TAGS])
{
    int fb = 0, pb = 0, ub = 0;
    memset(tag_count, 0, sizeof(int) * PU_NUM_TAGS);
    if (!mainzone) { *free_blocks = *purge_blocks = *used_blocks = 0; return; }

    hz_memblock_t *b;
    for (b = mainzone->blocklist.next; b != &mainzone->blocklist; b = b->next) {
        if (b->tag >= 0 && b->tag < PU_NUM_TAGS) tag_count[b->tag]++;
        if (b->tag == PU_FREE)             fb++;
        else if (b->tag >= PU_PURGELEVEL)  pb++;
        else                               ub++;
    }
    *free_blocks = fb; *purge_blocks = pb; *used_blocks = ub;
}

int  Z_FreeMemory(void);
unsigned int Z_ZoneSize(void);

static void hz_dump_fragmentation(const char *banner)
{
    if (!mainzone) { printf("[probe] %s: mainzone NULL\n", banner); return; }

    int payload = 0;
    int run = hz_largest_reclaimable(&payload);
    int free_blocks, purge_blocks, used_blocks;
    int tag_count[PU_NUM_TAGS];
    hz_block_census(&free_blocks, &purge_blocks, &used_blocks, tag_count);
    int total_free = Z_FreeMemory();

    printf("==== ZONE FRAGMENTATION (%s) ====\n", banner);
    printf("  zone total        : %u bytes (%u KiB)\n",
           Z_ZoneSize(), Z_ZoneSize() / 1024);
    printf("  total reclaimable : %d bytes (%d KiB)   [Z_FreeMemory: free+purgeable]\n",
           total_free, total_free / 1024);
    printf("  LARGEST contiguous reclaimable run : %d bytes (payload-usable %d bytes)\n",
           run, payload);
    printf("  blocks: free=%d purgeable=%d used=%d  (header=%zu bytes)\n",
           free_blocks, purge_blocks, used_blocks, sizeof(hz_memblock_t));
    printf("  by tag: STATIC=%d SOUND=%d MUSIC=%d FREE=%d LEVEL=%d LEVSPEC=%d "
           "PURGELEVEL=%d CACHE=%d\n",
           tag_count[PU_STATIC], tag_count[PU_SOUND], tag_count[PU_MUSIC],
           tag_count[PU_FREE], tag_count[PU_LEVEL], tag_count[PU_LEVSPEC],
           tag_count[PU_PURGELEVEL], tag_count[PU_CACHE]);
    printf("=================================================\n");
    fflush(stdout);
}

/* ========================================================================= */
/* Z_Malloc call-site tracing via linker --wrap (no edits to z_zone.c).        */
/* We record a backtrace for every "large" Z_Malloc so we can name (a) the     */
/* allocation that fails -- I_Error never returns, so the LAST large call is   */
/* the failing one -- and (b) the top-N largest zone hogs during E1M5 setup.   */
/* ========================================================================= */
#define HZ_TRACE_THRESHOLD 2048   /* only backtrace allocs >= 2KB (cheap)     */
#define HZ_MAX_FRAMES      24
#define HZ_TOP_N           6

typedef struct {
    int   size;                   /* raw requested size (pre header/round)    */
    int   tag;
    int   has_user;
    int   nframes;
    void *frames[HZ_MAX_FRAMES];
    void *site[4];                /* __builtin_return_address fallback (qemu)  */
} hz_alloc_rec_t;

static hz_alloc_rec_t hz_last_big;              /* most recent large call      */
static int            hz_last_big_valid = 0;
static hz_alloc_rec_t hz_top[HZ_TOP_N];         /* largest seen, desc by size  */
static int            hz_top_count = 0;

extern void *__real_Z_Malloc(int size, int tag, void *user);

/* glibc backtrace() returns nothing under qemu-user; site[] is filled by the
 * caller (__wrap_Z_Malloc) via __builtin_return_address so we always have at
 * least the direct call site to resolve with addr2line. */
static void hz_capture(hz_alloc_rec_t *r, int size, int tag, void *user,
                       void *const site[4])
{
    r->size = size;
    r->tag = tag;
    r->has_user = (user != NULL);
    r->nframes = backtrace(r->frames, HZ_MAX_FRAMES);
    for (int i = 0; i < 4; i++) r->site[i] = site[i];
}

static void hz_top_insert(const hz_alloc_rec_t *r)
{
    if (hz_top_count < HZ_TOP_N) {
        hz_top[hz_top_count++] = *r;
    } else if (r->size > hz_top[HZ_TOP_N - 1].size) {
        hz_top[HZ_TOP_N - 1] = *r;
    } else {
        return;
    }
    /* keep sorted descending (tiny array, insertion-ish) */
    for (int i = hz_top_count - 1; i > 0; i--)
        if (hz_top[i].size > hz_top[i - 1].size) {
            hz_alloc_rec_t t = hz_top[i]; hz_top[i] = hz_top[i - 1]; hz_top[i - 1] = t;
        }
}

void *__wrap_Z_Malloc(int size, int tag, void *user)
{
    if (size >= HZ_TRACE_THRESHOLD) {
        /* Direct call site = caller of __wrap_Z_Malloc (the DOOM function). */
        void *site[4] = {
            __builtin_return_address(0),
            __builtin_extract_return_addr(__builtin_return_address(1)),
            __builtin_extract_return_addr(__builtin_return_address(2)),
            __builtin_extract_return_addr(__builtin_return_address(3)),
        };
        hz_capture(&hz_last_big, size, tag, user, site);
        hz_last_big_valid = 1;
        hz_top_insert(&hz_last_big);
    }
    return __real_Z_Malloc(size, tag, user);
}

/* ------------------------------------------------------------------------- */
/* WAD flash-XIP path -- exercises the REAL device source.                    */
/*                                                                            */
/* The screen-melt removal is now the real Core/Src/porting/doom/f_wipe.c     */
/* override (no-op hard cut), compiled by Makefile.doom -- no shim here.       */
/*                                                                            */
/* The flash-resident-WAD path is the real Core/Src/porting/doom/w_wad.c:     */
/* W_AddFile sets `wad_file->mapped = g_doom_wad_mapped` when                  */
/* `g_doom_wad_mapped != NULL && wad_file->length == g_doom_wad_size`, after  */
/* which W_CacheLumpNum serves every lump from the mapping with no Z_Malloc.   */
/* On the device main_doom.c defines these two globals and fills them via      */
/* odroid_overlay_cache_file_in_flash(); the harness does NOT compile          */
/* main_doom.c, so we define them here and publish an mmap() of the WAD        */
/* (PROT_READ) as the host analog of the device's XIP flash cache.            */
/* DOOM_WAD_MAP=0 leaves them NULL (control: real w_wad.c falls back to SD     */
/* reads -> Z_Malloc copies -> the failure returns).                          */
/* ------------------------------------------------------------------------- */
uint8_t  *g_doom_wad_mapped = NULL;   /* consumed by the REAL Core/.../w_wad.c */
uint32_t  g_doom_wad_size   = 0;

static void hz_map_wad(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("[host] map WAD: open failed\n"); return; }
    off_t len = lseek(fd, 0, SEEK_END);
    void *base = mmap(NULL, (size_t)len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED) { printf("[host] map WAD: mmap failed\n"); return; }

    g_doom_wad_mapped = (uint8_t *)base;
    g_doom_wad_size   = (uint32_t)len;
    printf("[host] WAD flash-mapped at %p (size=%u) -- real w_wad.c will serve lumps from it\n",
           base, g_doom_wad_size);
}

static const char *hz_tag_name(int tag)
{
    switch (tag) {
        case PU_STATIC:     return "PU_STATIC";
        case PU_SOUND:      return "PU_SOUND";
        case PU_MUSIC:      return "PU_MUSIC";
        case PU_FREE:       return "PU_FREE";
        case PU_LEVEL:      return "PU_LEVEL";
        case PU_LEVSPEC:    return "PU_LEVSPEC";
        case PU_PURGELEVEL: return "PU_PURGELEVEL";
        case PU_CACHE:      return "PU_CACHE";
        default:            return "?";
    }
}

static void hz_print_rec(const char *label, const hz_alloc_rec_t *r)
{
    printf("%s size=%d bytes  tag=%s(%d)  user=%s\n",
           label, r->size, hz_tag_name(r->tag), r->tag,
           r->has_user ? "yes" : "NULL");
    printf("  raw frame addresses (for addr2line -fe ./build_doom/retro-go-doom.elf):\n   ");
    for (int i = 0; i < r->nframes; i++) printf(" %p", r->frames[i]);
    printf("\n  __builtin_return_address call sites (addr2line these):\n   ");
    for (int i = 0; i < 4; i++) printf(" %p", r->site[i]);
    printf("\n  backtrace_symbols:\n");
    char **syms = backtrace_symbols((void *const *)r->frames, r->nframes);
    if (syms) {
        for (int i = 0; i < r->nframes; i++) printf("    #%d %s\n", i, syms[i]);
        free(syms);
    }
    fflush(stdout);
}

static void hz_print_alloc_report(void)
{
    printf("\n======== Z_Malloc CALL-SITE TRACE ========\n");
    if (hz_last_big_valid) {
        hz_print_rec("[FAILING / most-recent large Z_Malloc]", &hz_last_big);
    } else {
        printf("(no large Z_Malloc recorded)\n");
    }
    printf("\n---- TOP %d largest Z_Malloc calls (>= %d B) this run ----\n",
           hz_top_count, HZ_TRACE_THRESHOLD);
    for (int i = 0; i < hz_top_count; i++) {
        char hdr[48];
        snprintf(hdr, sizeof(hdr), "[#%d top alloc]", i + 1);
        hz_print_rec(hdr, &hz_top[i]);
    }
    printf("==========================================\n");
    fflush(stdout);
}

/* ========================================================================= */
/* Bonus pool (LUT8 framebuffer pool on device): I_VideoBuffer (64000 B) and  */
/* the WAD lumpinfo come from HERE, NOT the zone -- this is why the device     */
/* zone budget is "pure game data". Bump allocator, malloc fallback (device    */
/* falls back to ram_malloc). doom_bonus_used/size are read by I_Error's MEM   */
/* report in doom_i_system.c.                                                  */
/* ========================================================================= */
static uint8_t *doom_bonus_base = NULL;
size_t          doom_bonus_size = 0;
size_t          doom_bonus_used = 0;

void *doom_bonus_alloc(size_t n)
{
    n = (n + 3u) & ~(size_t)3u;
    if (doom_bonus_base && doom_bonus_used + n <= doom_bonus_size) {
        void *p = doom_bonus_base + doom_bonus_used;
        doom_bonus_used += n;
        return p;
    }
    return malloc(n);   /* fallback: still OUT of the zone */
}

/* ========================================================================= */
/* G&W HAL stubs referenced by the DOOM override files.                       */
/* ========================================================================= */

/* doom_i_system.c I_ZoneBase: zone is sized from ram_get_free_size()-16KiB.
 * Return zone_kb*1024 + 16KiB so the zone comes out to exactly zone_kb KiB. */
unsigned int ram_get_free_size(void)
{
    return (unsigned int)zone_kb * 1024u + 16u * 1024u;
}
void *ram_malloc(size_t n) { return malloc(n); }

/* I_Error (doom_i_system.c) calls these after printing its MEM line. We hook
 * gw_debug_show_log to emit the full fragmentation fact-dump at the failure. */
void wdog_refresh(void) {}   /* watchdog kick (w_wad.c override) -- no-op */
void lcd_setup_framebuffers(int mode) { (void)mode; }
void lcd_set_clut(const uint32_t *clut, unsigned short count) { (void)clut; (void)count; }
void gw_debug_show_log(const char *banner)
{
    printf("[doom] >> I_Error banner: %s\n", banner ? banner : "(null)");
    hz_dump_fragmentation("AT Z_Malloc FAILURE");
    hz_print_alloc_report();
}

/* ========================================================================= */
/* doomgeneric DG_* platform callbacks (headless).                            */
/* ========================================================================= */
static uint32_t dg_ticks_ms = 0;

void DG_Init(void) {}
void DG_DrawFrame(void) {}                 /* headless: no blit */
void DG_SleepMs(uint32_t ms) { (void)ms; }
uint32_t DG_GetTicksMs(void)
{
    /* Advance ~one tic (1000/35 ms) per call so TryRunTics always has time to
     * run the demo forward; called a few times per Tick => demo plays fast. */
    dg_ticks_ms += 29;
    return dg_ticks_ms;
}
int DG_GetKey(int *pressed, unsigned char *key) { (void)pressed; (void)key; return 0; }
void DG_SetWindowTitle(const char *title) { (void)title; }

/* ========================================================================= */
/* Driver.                                                                    */
/* ========================================================================= */
static int env_int(const char *name, int dflt)
{
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    int x = atoi(v);
    return x > 0 ? x : dflt;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IOLBF, 0);

    zone_kb  = env_int("DOOM_ZONE_KB",  DEFAULT_ZONE_KB);
    bonus_kb = env_int("DOOM_BONUS_KB", DEFAULT_BONUS_KB);
    int max_ticks = env_int("DOOM_MAX_TICKS", DEFAULT_MAX_TICKS);
    /* DOOM_WAD_MAP=1 (default): publish g_doom_wad_mapped so the REAL w_wad.c
     * takes its flash-XIP path. =0: leave NULL (control -> SD-read fallback). */
    const char *wm = getenv("DOOM_WAD_MAP");
    int wad_map = (!wm || !*wm || *wm != '0') ? 1 : 0;
    const char *wad = getenv("DOOM_WAD");
    if (!wad || !*wad) wad = DEFAULT_WAD;

    printf("######## DOOM HOST HARNESS  zone=%dK bonus=%dK max_ticks=%d wad_map=%d ########\n",
           zone_kb, bonus_kb, max_ticks, wad_map);
    printf("WAD: %s\n", wad);

    /* Allocate the bonus pool (matches device freed-framebuffer pool). */
    doom_bonus_size = (size_t)bonus_kb * 1024u;
    doom_bonus_base = (uint8_t *)malloc(doom_bonus_size);
    doom_bonus_used = 0;
    if (!doom_bonus_base) { fprintf(stderr, "bonus pool malloc failed\n"); return 2; }

    /* Publish the mapped-WAD globals BEFORE D_DoomMain's W_AddFile runs, so the
     * real w_wad.c picks them up (device: app_main_doom caches to flash first). */
    if (wad_map) hz_map_wad(wad);
    else printf("[host] WAD map DISABLED (control) -- real w_wad.c falls back to SD reads\n");

    /* -nogui: I_Error (doom_i_system.c) otherwise tries to pop a zenity dialog. */
    char *dargv[] = { "doom", "-nogui", "-iwad", (char *)wad };
    doomgeneric_Create(4, dargv);   /* runs D_DoomMain: zone init, title, demo */
    printf("[host] doomgeneric_Create returned (engine initialised)\n");

    /* Headless run: drive the attract loop. Track the TIGHTEST moment seen
     * (smallest largest-reclaimable-run) -- that is the critical moment for a
     * successful run. If Z_Malloc fails, I_Error -> gw_debug_show_log dumps the
     * facts and exit(-1) terminates us before we get back here. */
    int min_run = 0x7fffffff, min_payload = 0, min_tick = -1, min_free = 0;
    int reached_e1m5 = 0;
    int prev_map = -1;

    for (int t = 0; t < max_ticks; t++) {
        doomgeneric_Tick();

        if (gamemap != prev_map) {
            printf("[host] tick %d: gamemap -> %d%s\n", t, gamemap,
                   gamemap == 5 ? "  (E1M5)" : "");
            prev_map = gamemap;
            if (gamemap == 5) reached_e1m5 = 1;
        }

        int payload = 0;
        int run = hz_largest_reclaimable(&payload);
        if (run < min_run) {
            min_run = run; min_payload = payload; min_tick = t;
            min_free = Z_FreeMemory();
        }
    }

    printf("\n######## RUN COMPLETE (no Z_Malloc failure) ########\n");
    printf("RESULT: zone=%dK  E1M5_loaded=%s\n",
           zone_kb, reached_e1m5 ? "YES" : "NO");
    printf("Tightest moment: tick=%d  largest-reclaimable-run=%d B "
           "(payload %d B)  total-free=%d B (%d KiB)\n",
           min_tick, min_run, min_payload, min_free, min_free / 1024);
    hz_dump_fragmentation("END OF RUN");
    hz_print_alloc_report();
    return reached_e1m5 ? 0 : 3;
}
