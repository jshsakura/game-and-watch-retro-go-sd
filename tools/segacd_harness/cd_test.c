/* Host validation of the Sega CD storage backend (segacd_cd.c) against REAL
 * discs. Includes the .c directly so it can reach the static TOC/read_sector.
 *
 *   cc cd_test.c -I<gwenesis M68K> -I<segacd> -o cd_test && ./cd_test game.cue
 *
 * Checks: cue parses into tracks; sector 0 of the data track carries the Sega
 * CD signature "SEGADISCSYSTEM"/"SEGABOOTDISC" (proves 2352->2048 offset + read
 * are right); an AUDIO track reads as CD-DA. This is the same code the device
 * links — a real test of the real file, PCE-harness style.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Minimal SCD global so segacd_cd.c links without the whole engine. */
#include "segacd.h"
segacd_state SCD;

#include "segacd_cd.c"   /* pulls in static CD, read_sector, track_at_lba */

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: cd_test <game.cue>\n"); return 2; }

    if (segacd_cd_open(argv[1]) != 0) {
        fprintf(stderr, "FAIL: could not open/parse cue: %s\n", argv[1]);
        return 1;
    }

    printf("TOC: %d tracks, total_lba=%u\n", CD.num_tracks, CD.total_lba);
    for (int i = 0; i < CD.num_tracks; i++) {
        cd_track_t *t = &CD.tracks[i];
        printf("  track %02d  %-5s  start_lba=%-7u  size=%u  bin=%s\n",
               i + 1, t->is_audio ? "AUDIO" : "DATA", t->start_lba,
               t->sector_size, t->bin_path);
    }

    /* Sega CD boot signature at data-track sector 0 (user bytes). */
    uint8_t sec[2048];
    if (read_sector(0, sec, 2048) != 0) {
        fprintf(stderr, "FAIL: read_sector(0) failed\n");
        return 1;
    }
    char sig[17]; memcpy(sig, sec, 16); sig[16] = 0;
    printf("sector0[0..15] = \"%s\"\n", sig);
    int ok = (memcmp(sec, "SEGADISCSYSTEM", 14) == 0) ||
             (memcmp(sec, "SEGABOOTDISC",  12) == 0) ||
             (memcmp(sec, "SEGA",           4) == 0);
    printf("Sega CD signature: %s\n", ok ? "FOUND ✓" : "NOT FOUND ✗");

    /* First audio track -> CD-DA read smoke test. */
    for (int i = 0; i < CD.num_tracks; i++) {
        if (CD.tracks[i].is_audio) {
            segacd_cdda_play(CD.tracks[i].start_lba);
            int16_t pcm[588 * 2];
            int n = segacd_cdda_fill(pcm, 588);
            long peak = 0;
            for (int s = 0; s < n * 2; s++) { long a = pcm[s]; if (a < 0) a = -a; if (a > peak) peak = a; }
            printf("CD-DA track %02d: filled %d frames, peak=%ld\n", i + 1, n, peak);
            break;
        }
    }

    return ok ? 0 : 1;
}
