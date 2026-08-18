/* Native host driver for the 32X core: the same picodrive sources the device
 * overlay and the QEMU rig compile, running at host speed so an input sequence
 * that actually reaches a level can be searched for in seconds instead of five
 * minutes a try. Prints a per-frame framebuffer checksum and unique-colour
 * count; the rig then replays whatever sequence works. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "pico/pico_types.h"
#include "pico/pico.h"
#include "pico/pico_int.h"

static unsigned short fb[320 * 240];
static short snd[4096];
static void wr_snd(int len) { (void)len; }

#define PAD_UP (1u<<0)
#define PAD_DOWN (1u<<1)
#define PAD_LEFT (1u<<2)
#define PAD_RIGHT (1u<<3)
#define PAD_B (1u<<4)
#define PAD_C (1u<<5)
#define PAD_A (1u<<6)
#define PAD_START (1u<<7)

/* Input patterns to search for one that actually starts a level. The QEMU rig
 * takes five minutes a try; here it is a second, which is the whole point of
 * this driver. */
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
    {
        const char *out = getenv("FB_OUT");
        if (out) { FILE *g = fopen(out, "wb"); if (g) { fwrite(fb, 2, 320 * 240, g); fclose(g); } }
    }
    return 0;
}

/* Fired by the lazy Pico32xStartup when the game's 68K writes ADEN.
 * PicoDrawSetOutFormat/PicoDrawSetOutBuf route to their 32X variants only once
 * PAHW_32X is set, so they must be re-applied here or the 32X layer renders
 * into picodrive's internal buffer and the screen stays black. */
void emu_32x_startup(void)
{
    printf("[host] emu_32x_startup fired\n");
    PicoDrawSetOutFormat(PDF_RGB555, 0);
    PicoDrawSetOutBuf(fb, 320 * 2);
}

int main(int argc, char **argv)
{
    const char *rom = argc > 1 ? argv[1] : NULL;
    int frames = argc > 2 ? atoi(argv[2]) : 600;
    const char *pat = argc > 3 ? argv[3] : "none";
    unsigned char *data; long sz; FILE *f; int i;

    if (!rom) { fprintf(stderr, "usage: host_drv <rom.32x> [frames]\n"); return 2; }
    f = fopen(rom, "rb"); if (!f) { perror("rom"); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    data = malloc(sz); if (fread(data, 1, sz, f) != (size_t)sz) return 2;
    fclose(f);

    PicoInit();
    /* Mirrors tools/m7_qemu_rig/rig_32x.c exactly. Its own comment warns that a
     * partial init is what hung the QEMU rig on the first PicoFrame, and an
     * earlier version of this driver reproduced that: the 68K never wrote ADEN,
     * emu_32x_startup never fired, and 600 frames came out blank. */
    PicoIn.opt = POPT_EN_FM | POPT_EN_PSG | POPT_EN_Z80
               | POPT_EN_32X | POPT_EN_PWM
               | POPT_ACC_SPRITES | POPT_DIS_32C_BORDER;   /* mono: no EN_STEREO */
    PicoIn.sndRate = 44100;
    PicoIn.autoRgnOrder = 0x184;   /* US, EU, JP */

    if (PicoLoadMedia(rom, data, sz, NULL, NULL, NULL, NULL) == PM_ERROR) {
        fprintf(stderr, "load failed\n"); return 2;
    }
    printf("[host] AHW=%x romsize=%u\n", (unsigned)PicoIn.AHW, (unsigned)Pico.romsize);

    PicoLoopPrepare();
    PicoIn.sndOut = snd;
    PicoIn.writeSound = wr_snd;
    PsndRerate(0);
    PicoDrawSetOutFormat(PDF_RGB555, 0);
    PicoDrawSetOutBuf(fb, 320 * 2);

    /* pad pattern: argv[3] = "none" | "amash" | "amash+start" ... */
    for (i = 0; i < frames; i++) {
        PicoIn.pad[0] = pad_for(i, pat);
        PicoFrame();
        if ((i % 20) == 0 || i == frames - 1) {
            uint32_t ck = 0; int nz = 0, j;
            for (j = 0; j < 320 * 240; j++) { ck = ck * 131 + fb[j]; nz |= fb[j] != 0; }
            int cols = 0, seen[64]; int k;
            for (k = 0; k < 64; k++) seen[k] = 0;
            for (j = 0; j < 320 * 240; j += 7) { int b = fb[j] & 63; if (!seen[b]) { seen[b] = 1; cols++; } }
            printf("f%04d ck=%08x nb=%d col=%d\n", i, ck, nz, cols);
        }
    }
    {
        const char *out = getenv("FB_OUT");
        if (out) {
            FILE *g = fopen(out, "wb");
            if (g) { fwrite(fb, 2, 320 * 240, g); fclose(g); }
        }
    }
    return 0;
}
