/* Native host driver for the Sega CD core: the same picodrive sources the
 * device overlay compiles (SEGACD_C_SOURCES minus main_segacd.c), with the
 * device's defines, running at host speed so a converted cue/bin disc proves
 * it boots in seconds instead of a device flash cycle. Prints per-frame
 * framebuffer checksums plus cdd status/lba so "black screen" can be told
 * apart from "seeking".
 *
 * Mirrors app_main_segacd() in Core/Src/porting/segacd/main_segacd.c: same
 * PicoIn.opt, same region->BIOS pick, same gnw_mcd_bios_xip setup, same
 * PicoLoadMedia call, same PDF_8BIT output buffer.
 *
 * NOT a performance instrument; see tools/pico_host/run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "pico/pico_types.h"
#include "pico/pico.h"
#include "pico/pico_int.h"
#include "pico/memory.h"
#include "pico/cd/genplus_macros.h"   /* uint8/u16/u32 -> picodrive's u8/u16/u32 */
#include "pico/cd/cdd.h"
#include "porting/segacd/segacd_boot_start.h"
#include "porting/segacd/segacd_config.h"

static unsigned char fb[320 * 240];   /* PDF_8BIT: one byte per pixel */

/* The device's border rule (main_segacd.c): a mode change wipes the frame. */
void emu_video_mode_change(int sl, int lc, int sc, int cc)
{
    (void)sl; (void)lc; (void)sc; (void)cc;
    memset(fb, SEGACD_BORDER_INDEX, sizeof(fb));
}
static short snd[2048];
static void wr_snd(int len) { (void)len; }

#define PAD_UP (1u<<0)
#define PAD_DOWN (1u<<1)
#define PAD_LEFT (1u<<2)
#define PAD_RIGHT (1u<<3)
#define PAD_B (1u<<4)
#define PAD_C (1u<<5)
#define PAD_A (1u<<6)
#define PAD_START (1u<<7)

/* Input patterns, same vocabulary as tools/pico_host: find a pad sequence that
 * gets past the BIOS splash / title screen and into gameplay. */
static segacd_boot_start_t boot;

static unsigned short pad_for(int f, const char *pat)
{
    if (!pat || !strcmp(pat, "none")) return 0;
    if (!strcmp(pat, "amash"))   return (f % 12) < 6 ? PAD_A : 0;
    if (!strcmp(pat, "smash"))   return (f % 12) < 6 ? PAD_START : 0;
    if (!strcmp(pat, "cmash"))   return (f % 12) < 6 ? PAD_C : 0;
    if (!strcmp(pat, "s_then_a")) {
        if (f >= 120 && f < 132) return PAD_START;
        if (f >= 240 && f < 252) return PAD_A;
        return 0;
    }
    if (!strcmp(pat, "slow_a"))  return (f % 40) < 8 ? PAD_A : 0;
    /* JP/EU Mega-CD BIOS parks at its menu until START, even with a disc in;
     * the US BIOS auto-boots. Press START twice, then behave like "play". */
    if (!strcmp(pat, "jplay")) {
        if ((f >= 300 && f < 312) || (f >= 420 && f < 432)) return PAD_START;
        return pad_for(f + 200, "play");
    }
    /* What the device does: segacd_boot_start presses START through the
     * BIOS menu, then "play" once the game owns the pad. */
    if (!strcmp(pat, "auto")) {
        unsigned short p = segacd_boot_start_pad(&boot, SekPc);
        return boot.active ? p : pad_for(f + 200, "play");
    }
    /* The device with nobody at the buttons: START through the BIOS menu,
     * then no input, so the game's own attract/demo loop runs. */
    if (!strcmp(pat, "idle")) {
        unsigned short p = segacd_boot_start_pad(&boot, SekPc);
        return boot.active ? p : 0;
    }
    if (!strcmp(pat, "play")) {
        if (f < 660) return (f % 12) < 6 ? PAD_A : 0;
        switch ((f / 30) % 4) {
        case 0: return PAD_UP;
        case 1: return PAD_UP | PAD_RIGHT;
        case 2: return PAD_UP | PAD_LEFT;
        default: return PAD_UP | PAD_C;
        }
    }
    return 0;
}

/* Region codes per PicoCdCheck: 8 = EU, 1/2 = JAP, else US. Device mounts the
 * same files at /bios/segacd/; here they live in BIOS_DIR. */
static const char *bios_name_for_region(int region)
{
    if (region == 8) return "bios_CD_E.bin";
    if (region == 1 || region == 2) return "bios_CD_J.bin";
    return "bios_CD_U.bin";
}

static unsigned char *load_bios(const char *dir, const char *name, long *out_sz)
{
    char path[512];
    unsigned char *buf;
    long sz;
    FILE *f = fopen(snprintf(path, sizeof(path), "%s/%s", dir, name) < 0 ? "" : path, "rb");
    if (!f) { fprintf(stderr, "[host] BIOS open failed: %s/%s\n", dir, name); return NULL; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0x20000) { fclose(f); fprintf(stderr, "[host] BIOS short: %ld\n", sz); return NULL; }
    buf = malloc(sz);
    if (!buf || fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
    fclose(f);
    /* The device caches the BIOS with odroid_overlay_cache_file_in_flash(..,
     * byte_swap=true) -- the core expects host-order words (LSB_FIRST build).
     * Files on disk are big-endian, so swap here or the reset vector reads
     * garbage and the main 68k wanders around 0xffffxxxx executing RAM. */
    Byteswap(buf, buf, (int)sz);
    *out_sz = sz;
    return buf;
}

/* Non-split build: PicoLoadMedia asks the frontend for the BIOS path (the
 * upstream flow); answer with the region-appropriate file, same as the split
 * path's load_bios choice. */
static const char *host_bios_filename(int *region, const char *cd_fname)
{
    static char path[512];
    const char *dir = getenv("BIOS_DIR");
    if (!dir) dir = "/media/pi/EXTERNAL/BIOS";
    snprintf(path, sizeof(path), "%s/%s", dir, bios_name_for_region(*region));
    printf("[host] bios file for region %d: %s\n", *region, path);
    return path;
}

/* The firmware wraps picodrive's state stream in an 8-byte header (magic
 * SEGACD_STATE_MAGIC 0x53434450, then a version word) -- see segacd_save_state
 * in Core/Src/porting/segacd/main_segacd.c. Skip it and hand the rest to
 * PicoStateFP, exactly as segacd_load_state does. */
extern int PicoStateFP(void *afile, int is_save,
                       size_t (*read)(void *, size_t, size_t, void *),
                       size_t (*write)(void *, size_t, size_t, void *),
                       size_t (*eof)(void *),
                       int (*seek)(void *, long, int));

static size_t st_read(void *p, size_t sz, size_t n, void *f) { return fread(p, sz, n, (FILE *)f); }
static size_t st_write(void *p, size_t sz, size_t n, void *f) { return fwrite(p, sz, n, (FILE *)f); }
static size_t st_eof(void *f) { return (size_t)feof((FILE *)f); }
static int    st_seek(void *f, long o, int w) { return fseek((FILE *)f, o, w); }

static int load_device_state(const char *path)
{
    unsigned int hdr[2];
    FILE *f = fopen(path, "rb");
    int rc;

    if (!f) { perror("state"); return -1; }
    if (fread(hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return -1; }
    printf("[host] state magic=%08x ver=%u\n", hdr[0], hdr[1]);
    rc = PicoStateFP(f, 0, st_read, st_write, st_eof, st_seek);
    fclose(f);
    printf("[host] PicoStateFP -> %d\n", rc);
    return rc;
}

int main(int argc, char **argv)
{
    const char *rom = argc > 1 ? argv[1] : NULL;
    int frames = argc > 2 ? atoi(argv[2]) : 600;
    const char *pat = argc > 3 ? argv[3] : "none";
    const char *bios_dir = getenv("BIOS_DIR");
    const char *state = getenv("STATE_IN");
    /* Save/load round trip: STATE_OUT is written after SAVE_AT frames in the
     * device's format (magic+version, then PicoStateFP). FRAME0 shifts the
     * printed frame numbers and the pad pattern so a resumed run lines up
     * frame for frame with an uninterrupted one. */
    const char *state_out = getenv("STATE_OUT");
    int save_at = getenv("SAVE_AT") ? atoi(getenv("SAVE_AT")) : -1;
    int frame0 = getenv("FRAME0") ? atoi(getenv("FRAME0")) : 0;
    int region = 4, i;
    long bsz = 0;
    unsigned char *bios;
    enum media_type_e mt;

    if (!rom) { fprintf(stderr, "usage: host_mcd <game.cue> [frames] [pad-pattern]\n"); return 2; }
    if (!bios_dir) bios_dir = "/media/pi/EXTERNAL/BIOS";

    PicoInit();
    PicoIn.opt = SEGACD_PICO_OPT;   /* the device's own set, segacd_config.h */
    PicoIn.sndRate = 44100;
    PicoIn.autoRgnOrder = 0x184;   /* US, EU, JP */

    (void)PicoCdCheck(rom, &region);
#ifdef GNW_MCD_SPLIT
    bios = load_bios(bios_dir, bios_name_for_region(region), &bsz);
    if (!bios) return 2;
    printf("[host] region=%d bios=%s/%s (%ld bytes)\n",
           region, bios_dir, bios_name_for_region(region), bsz);

    /* The device XIPs the BIOS out of flash and points the core at it; the
     * host just owns a plain heap copy the core can memcpy its low 64K from. */
    gnw_mcd_bios_xip = bios;
    gnw_mcd_bios_xip_size = (unsigned int)bsz;

    mt = PicoLoadMedia(rom, NULL, 0, NULL, NULL, NULL, NULL);
#else
    /* Non-split model: let PicoLoadMedia load the BIOS itself through the
     * get_bios_filename callback (upstream path, PicoCartLoad byteswaps). */
    mt = PicoLoadMedia(rom, NULL, 0, NULL, host_bios_filename, NULL, NULL);
#endif
    if (mt == PM_ERROR || mt == PM_BAD_CD || mt == PM_BAD_CD_NO_BIOS) {
        fprintf(stderr, "[host] load failed (mt=%d)\n", (int)mt);
        return 2;
    }
    printf("[host] AHW=%x romsize=%u pal=%d tracks=%d cdd.loaded=%d\n",
           (unsigned)PicoIn.AHW, (unsigned)Pico.romsize,
           (int)Pico.m.pal, cdd.toc.last, cdd.loaded);

    /* Backup RAM persistence, same point as the device (after the media is in,
     * which powers the MCD and formats BRAM): BRAM_IN replaces the freshly
     * formatted 8K, BRAM_OUT dumps it after the run. */
    {
        const char *bin = getenv("BRAM_IN");
        FILE *g = bin ? fopen(bin, "rb") : NULL;
        if (g) {
            size_t n = fread(Pico_mcd->bram, 1, 0x2000, g);
            fclose(g);
            printf("[host] bram in: %zu bytes\n", n);
        }
    }

    PicoLoopPrepare();
    PicoSetInputDevice(0, PICO_INPUT_PAD_6BTN);
    PicoIn.sndOut = snd;
    PicoIn.writeSound = wr_snd;
    PsndRerate(0);

    /* Boot diagnostics: the reset vectors live at 0/4, and the first BIOS
     * instruction should sit under 0x20000. If this reads 0xffff..., the BIOS
     * never got mapped and the main 68K wanders open bus (seen once: every
     * frame black, PC in 0xffffxxxx, sub 68K never leaving 0). */
    {
        int q;
        printf("[host] bios[0..31] via m68k path:");
        for (q = 0; q < 32; q++) printf(" %02x", m68k_read8(q));
        printf("\n[host] bios[0..31] raw ptr   :");
        for (q = 0; q < 32; q++) printf(" %02x", Pico_mcd->bios[q]);
        printf("\n");
    }
    printf("[host] vec: SP=%06x PC=%06x (m68k path), map0=%llx\n",
           m68k_read16(0) << 16 | m68k_read16(2),
           m68k_read16(4) << 16 | m68k_read16(6),
           (unsigned long long)m68k_read16_map[0]);

    PicoDrawSetOutFormat(PDF_8BIT, 0);
    PicoDrawSetOutBuf(fb, 320);

    if (state && load_device_state(state) == 0) {
        /* what segacd_load does after a successful load */
        memset(fb, SEGACD_BORDER_INDEX, sizeof(fb));
        PicoDrawSetOutBuf(fb, 320);
    }

    memset(fb, SEGACD_BORDER_INDEX, sizeof(fb));
    segacd_boot_start_init(&boot, state == NULL);
    for (i = 0; i < frames; i++) {
        PicoIn.pad[0] = pad_for(i + frame0, pat);
        PicoFrame();
        /* DRAWONLY_AT=N: after frame N, repaint the way a menu does on the
         * device (segacd_repaint: PicoFrameDrawOnly, 60 times). Emulation
         * must continue exactly as if it had not happened. */
        if (getenv("DRAWONLY_AT") && i + 1 == atoi(getenv("DRAWONLY_AT"))) {
            int k;
            for (k = 0; k < 60; k++) PicoFrameDrawOnly();
            printf("[host] 60x PicoFrameDrawOnly after frame %d\n", i + 1);
        }
        if (state_out && i + 1 == save_at) {
            unsigned int hdr[2] = { 0x53434450u, 1u };
            FILE *g = fopen(state_out, "wb");
            int rc = -1;
            if (g && fwrite(hdr, sizeof(hdr), 1, g) == 1)
                rc = PicoStateFP(g, 1, st_read, st_write, st_eof, st_seek);
            if (g) fclose(g);
            printf("[host] saved state after %d frames -> %d\n", save_at, rc);
        }
        if ((i % 20) == 0 || i == frames - 1) {
            uint32_t ck = 0; int nz = 0, j;
            int cols = 0, seen[64]; int k;
            for (j = 0; j < 320 * 240; j++) { ck = ck * 131 + fb[j]; nz |= fb[j] != 0; }
            for (k = 0; k < 64; k++) seen[k] = 0;
            for (j = 0; j < 320 * 240; j += 7) { int b = fb[j] & 63; if (!seen[b]) { seen[b] = 1; cols++; } }
            printf("f%04d ck=%08x nb=%d col=%d cdd=%02x lba=%d pc=%06x pc2=%06x\n",
                   i + frame0, ck, nz, cols, (unsigned)cdd.status, cdd.lba,
                   (unsigned)SekPc, (unsigned)SekPcS68k);
            fflush(stdout);
        }
    }
    {
        const char *bout = getenv("BRAM_OUT");
        FILE *g = bout ? fopen(bout, "wb") : NULL;
        if (g) { fwrite(Pico_mcd->bram, 1, 0x2000, g); fclose(g); }
    }
    {
        const char *out = getenv("FB_OUT");
        if (out) {
            FILE *g = fopen(out, "wb");
            if (g) { fwrite(fb, 1, 320 * 240, g); fclose(g); }
        }
        /* Palette alongside (like segacd_push_palette reads it): 256x RGB555.
         * Combine with the fb dump to actually look at the screen. */
        out = getenv("PAL_OUT");
        if (out) {
            FILE *g = fopen(out, "wb");
            PicoDrawUpdateHighPal();
            if (g) { fwrite(Pico.est.HighPal, 2, 256, g); fclose(g); }
        }
    }
    return 0;
}
