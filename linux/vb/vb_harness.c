/*
 * Virtual Boy DEVICE-PATH host harness.
 *
 * Compiles the red-viper core with -DGNW_VB_DEVICE (flash-XIP ROM masking +
 * single-player RAM) under ASan+UBSan and exercises the exact code paths the
 * STM32H7 firmware will run — WITHOUT any 3DS/SDL/GL platform layer — so most
 * device bugs (ROM mask, single-player memory, savestate completeness, OOB) are
 * caught on the host before flashing. Mirrors linux/lynx/run_lynx_host_test.sh.
 *
 * Tests, all under the sanitizers (any ASan/UBSan error aborts non-zero):
 *   1. Boot: single-player init + flash-XIP ROM setup + reset + N frames.
 *   2. Save/Load round-trip: save S1 -> run (diverge) -> load S1 -> save S2;
 *      assert byte-identical S1==S2 (the real vb_savestate.c device module).
 *
 * Synthetic ROM only (no copyrighted VB data): a power-of-2 image whose reset
 * vector is a JR-to-self, enough to spin the VIP/CPU for real frames.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "vb_set.h"
#include "v810_cpu.h"
#include "v810_mem.h"
#include "vb_dsp.h"

/* tDSPCACHE normally lives in video.c (the GLES2 renderer we exclude on the
 * headless harness); provide the definition so the CPU/mem units link. */
VB_DSPCACHE tDSPCACHE;

/* device save/load module under test */
int vb_state_save(const char *path);
int vb_state_load(const char *path);

/* set by our v810_mem.c GNW_VB_DEVICE patch (rom_size-1 flash-XIP mirror mask) */
extern unsigned int vb_rom_mask;

/* Device RAM allocators — on the harness they map to the C library so the
 * SAME core allocation path (v810_init regions, sound buffers) is exercised;
 * on device main_vb.c maps these to ram_calloc/ram_malloc (RAM_EMU heap). */
void *vb_dev_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
void *vb_dev_malloc(size_t size) { return malloc(size); }

/* --- platform stubs the core references but the harness doesn't drive --- */
int  drc_handleInterrupts(WORD cpsr, WORD *PC) { (void)cpsr; (void)PC; return 0; }
void drc_relocTable(void) {}
void video_download_vip(int drawn_fb) { (void)drawn_fb; }
/* MUST return success (non-zero): sound_init() frees the wave buffers if the
 * backend reports failure, and sound_update() then writes freed memory (ASan
 * heap-use-after-free). The device sound_init_backend must likewise succeed. */
int  sound_init_backend(int16_t **bufs) { (void)bufs; return 1; }
void sound_close_backend(void) {}
void sound_pause_backend(void) {} void sound_resume_backend(void) {}
void sound_push_backend(void) {}

#define ROM_SIZE 0x400  /* 1 KiB, power of two */

static uint8_t *make_synthetic_rom(void)
{
    /* Simulated flash: a JR-to-self at the reset offset. Reset PC 0xFFFFFFF0
     * masks to (0x07000000 | (0xFFFFFFF0 & (ROM_SIZE-1))) = ..0x3F0 -> off+0x3F0. */
    uint8_t *rom = calloc(1, ROM_SIZE);
    rom[0x3F0] = 0x00; rom[0x3F1] = 0xA8;  /* JR opcode, disp hi */
    rom[0x3F2] = 0x00; rom[0x3F3] = 0x00;  /* disp lo (=0 -> self loop) */
    return rom;
}

static void setup_rom_flash_xip(uint8_t *rom, uint32_t len)
{
    /* Exactly what app_main_vb() does on device with the getromdata() pointer. */
    V810_ROM1.pmemory = rom;
    V810_ROM1.lowaddr = 0x07000000;
    V810_ROM1.size    = len;
    V810_ROM1.highaddr = 0x07000000 + len - 1;
    V810_ROM1.off     = (size_t)rom - 0x07000000;
    vb_rom_mask       = len - 1;   /* power-of-2 mirror mask, no RAM buffer */
}

/* FNV-1a over all four RAM regions — proves a load actually restored RAM,
 * not just that two saves of the same (possibly truncated) blob agree. */
static uint32_t ram_crc(void)
{
    uint32_t h = 2166136261u;
    const V810_MEMORYFETCH *regs[4] = {
        &vb_state->V810_DISPLAY_RAM, &vb_state->V810_SOUND_RAM,
        &vb_state->V810_VB_RAM,      &vb_state->V810_GAME_RAM,
    };
    for (int r = 0; r < 4; r++) {
        uint32_t n = regs[r]->highaddr + 1 - regs[r]->lowaddr;
        const uint8_t *p = regs[r]->pmemory;
        for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    }
    return h;
}

/* Fill every RAM byte with a deterministic, region+offset-dependent pattern so
 * the round-trip test doesn't depend on the synthetic ROM writing RAM, and so a
 * partial save/restore of ANY region is detected. seed=0 acts as "corrupt". */
static void ram_fill(uint8_t seed)
{
    V810_MEMORYFETCH *regs[4] = {
        &vb_state->V810_DISPLAY_RAM, &vb_state->V810_SOUND_RAM,
        &vb_state->V810_VB_RAM,      &vb_state->V810_GAME_RAM,
    };
    for (int r = 0; r < 4; r++) {
        uint32_t n = regs[r]->highaddr + 1 - regs[r]->lowaddr;
        uint8_t *p = regs[r]->pmemory;
        for (uint32_t i = 0; i < n; i++)
            p[i] = (uint8_t)(seed + (r * 37u) + (i * 101u) + (i >> 8));
    }
}

static long fsize(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static uint8_t *fslurp(const char *p, long *out_n)
{
    long n = fsize(p);
    if (n < 0) return NULL;
    FILE *f = fopen(p, "rb");
    uint8_t *b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *out_n = n;
    return b;
}

int main(void)
{
    setDefaults();
    tVBOpt.CRC32 = 0xDEADBEEF;   /* savestate stamps + checks this */

    v810_init();
    replay_init();

    uint8_t *rom = make_synthetic_rom();
    setup_rom_flash_xip(rom, ROM_SIZE);

    v810_reset();
    clearCache();
    sound_init();

    /* 1. Boot: run real frames on the device path. */
    const int WARM = 120;   /* ~2.4s of VB time */
    for (int i = 0; i < WARM; i++) {
        int e = v810_run();
        if (e) { printf("FAIL boot: v810_run err %d at frame %d\n", e, i); return 1; }
    }
    printf("OK boot: %d frames on flash-XIP single-player device path\n", WARM);

    /* 2. Save/Load round-trip. */
    const char *S1 = "/tmp/vb_s1.ss";
    const char *S2 = "/tmp/vb_s2.ss";

    /* Put a known, non-trivial pattern across all RAM so the save captures real
     * content (independent of what the synthetic ROM does) and any dropped
     * region is caught. Also run a few frames so CPU/VIP regs are non-default. */
    ram_fill(0x5A);
    for (int i = 0; i < 4; i++) v810_run();
    uint32_t crc_at_save = ram_crc();

    if (vb_state_save(S1)) { printf("FAIL: vb_state_save(S1)\n"); return 1; }

    /* Truncation guard: the four RAM regions total ~338 KB; a savestate that
     * dropped DISPLAY_RAM/VB_RAM (e.g. a uint16_t size field) would be tiny. */
    long sz1 = fsize(S1);
    if (sz1 < 330000) {
        printf("FAIL: savestate is only %ld bytes — RAM not fully serialized\n", sz1);
        return 1;
    }
    printf("OK size: savestate is %ld bytes (all four RAM regions present)\n", sz1);

    /* Corrupt every RAM region, then load: proves load rewrites ALL of RAM. */
    ram_fill(0x00);
    if (ram_crc() == crc_at_save) { printf("FAIL: corrupt pattern collided with saved\n"); return 1; }
    if (vb_state_load(S1)) { printf("FAIL: vb_state_load(S1)\n"); return 1; }
    if (ram_crc() != crc_at_save) {
        printf("FAIL restore: RAM after load != RAM at save (incomplete/incorrect load)\n");
        return 1;
    }
    printf("OK restore: all RAM byte-exact after corrupt->load\n");

    if (vb_state_save(S2)) { printf("FAIL: vb_state_save(S2)\n"); return 1; }

    long n1 = 0, n2 = 0;
    uint8_t *b1 = fslurp(S1, &n1);
    uint8_t *b2 = fslurp(S2, &n2);
    if (!b1 || !b2) { printf("FAIL: cannot re-read savestates\n"); return 1; }
    if (n1 != n2 || memcmp(b1, b2, n1) != 0) {
        printf("FAIL round-trip: S1(%ld) != S2(%ld) — load did not fully restore state\n", n1, n2);
        return 1;
    }
    printf("OK round-trip: save->diverge->load->save byte-identical (%ld bytes)\n", n1);

    /* reject-wrong-ROM guard: a savestate with a different CRC must not load */
    tVBOpt.CRC32 = 0x12345678;
    if (vb_state_load(S1) == 0) { printf("FAIL: loaded a savestate for the wrong ROM\n"); return 1; }
    printf("OK guard: mismatched-CRC savestate correctly rejected\n");

    free(b1); free(b2); free(rom);
    printf("ALL VB DEVICE-PATH HARNESS TESTS PASSED\n");
    return 0;
}
