#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define OSC_NTSC 53693175
#define OSC_PAL  53203424
#define FREQ_SH  16
struct arm { const char *tag; int clock, rate; };
static int bad_for(int clock, int rate, int S, uint64_t *cq_out)
{
    double freqbase = rate ? ((double)clock / rate) / (6*24) : 0;
    double C = 32.0 * freqbase * (double)(1 << (FREQ_SH - 10));
    uint64_t Cq = (uint64_t)((long double)C * powl(2.0L, S) + 0.5L);
    *cq_out = Cq;
    int bad = 0;
    for (int i = 0; i < 4096; i++)
        if ((uint32_t)((double)i * C) != (uint32_t)(((uint64_t)i * Cq) >> S)) bad++;
    return bad;
}
int main(void)
{
    int nn = (OSC_NTSC/7 + 3*24)/(6*24), pn = (OSC_PAL/7 + 3*24)/(6*24);
    struct arm arms[] = {
        {"NTSC 44100", OSC_NTSC/7, 44100}, {"NTSC 22050", OSC_NTSC/7, 22050},
        {"NTSC 32000", OSC_NTSC/7, 32000}, {"NTSC 48000", OSC_NTSC/7, 48000},
        {"NTSC native", OSC_NTSC/7, nn},
        {"PAL 44100", OSC_PAL/7, 44100},   {"PAL 22050", OSC_PAL/7, 22050},
        {"PAL native", OSC_PAL/7, pn},
    };
    int n = sizeof(arms)/sizeof(arms[0]);
    printf("%-3s %-8s %-12s %s\n", "S", "totalbad", "maxCq", "fits u32? / per-arm bad");
    for (int S = 12; S <= 34; S++) {
        int total = 0; uint64_t maxcq = 0, cq; char detail[512] = "";
        for (int a = 0; a < n; a++) {
            int b = bad_for(arms[a].clock, arms[a].rate, S, &cq);
            total += b; if (cq > maxcq) maxcq = cq;
            if (b) sprintf(detail + strlen(detail), " %s:%d", arms[a].tag, b);
        }
        printf("%-3d %-8d %-12llu %-6s%s\n", S, total, (unsigned long long)maxcq,
               maxcq < 4294967296ULL ? "YES" : "no", detail);
    }
    return 0;
}
