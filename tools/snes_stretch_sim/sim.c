/* Run the REAL audio stretcher over REAL game audio, on a host, and write a WAV.
 *
 * Until now the one open question in the SNES audio work -- keep pitch and
 * accept splices, or follow the rate and accept a 5% transposition -- could
 * only be judged by flashing the device and listening. The counters existed;
 * the sound did not, off-device. That made every iteration a build, a flash,
 * and a person at the console with headphones.
 *
 * There is no reason for that. The QEMU rig already computes the exact samples
 * the device's snes_pcm_submit() hands the stretcher (it hashed them for the
 * audio gate and threw them away); RIG_AUDIO_DUMP writes them out instead. The
 * stretcher is plain C that already links on a host in tests/. All that was
 * missing between them was the clock.
 *
 * This supplies it: pushes arrive at the emulator's frame rate, pulls at the
 * audio DMA's 60.15 Hz, and the merge of those two event streams is exactly
 * what the device does. The output is a WAV anyone can listen to, plus the
 * same counters stretch_ab.sh reads over SWD.
 *
 *   sim <audio.pcm> <out.wav> [--fps 56.93 | --bimodal 14.6,32.4,4] [--follow]
 *
 * The default schedule is the measured one: frame times on hardware are
 * bimodal, 14.6 ms when the overload guard skips the render and 32.4 ms when it
 * draws, one drawn in four. A uniform --fps average is NOT the same stimulus --
 * the ring level swings by a whole frame across a drawn one -- so the bimodal
 * default is the honest reproduction and --fps is for sweeps.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../Core/Src/porting/snes/snes_audio_stretch.h"

#define FRAME      266u          /* SNES_AUDIO_SAMPLES */
#define PERIOD_HZ  60.15         /* audio DMA: one half-buffer per period */

extern uint32_t g_stretch_ins, g_stretch_pulls, g_stretch_noise_pulls;
extern uint32_t g_stretch_rev;

static void write_wav(const char *path, const int16_t *pcm, size_t n, uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    uint32_t data = (uint32_t)(n * 2), riff = 36 + data, brate = rate * 2;
    uint16_t one = 1, two = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    uint32_t sixteen = 16; fwrite(&sixteen, 4, 1, f);
    fwrite(&one, 2, 1, f); fwrite(&one, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&brate, 4, 1, f);
    fwrite(&two, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(pcm, 2, n, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: sim <audio.pcm> <out.wav> "
                    "[--fps F | --bimodal fast,slow,every] [--follow]\n"); return 2; }
    const char *in = argv[1], *out = argv[2];
    double fps = 0, t_fast = 14.6, t_slow = 32.4; int every = 4;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--bimodal") && i + 1 < argc)
            sscanf(argv[++i], "%lf,%lf,%d", &t_fast, &t_slow, &every);
        else if (!strcmp(argv[i], "--follow")) g_snes_audio_gapfree = 1;
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }

    FILE *f = fopen(in, "rb");
    if (!f) { perror(in); return 1; }
    fseek(f, 0, SEEK_END); long bytes = ftell(f); fseek(f, 0, SEEK_SET);
    size_t frames = (size_t)bytes / (FRAME * 2);
    int16_t *src = malloc(frames * FRAME * 2);
    if (fread(src, 2, frames * FRAME, f) != frames * FRAME) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    snes_stretch_reset();

    /* Two event streams on one clock, in milliseconds. */
    const double t_period = 1000.0 / PERIOD_HZ;
    size_t out_cap = (size_t)(frames * 1.4) * FRAME + FRAME;
    int16_t *dst = malloc(out_cap * 2), *p = dst;
    size_t pulls = 0, pushed = 0;
    double t_push = 0, t_pull = t_period;

    while (pushed < frames) {
        if (t_push <= t_pull) {
            snes_stretch_push(src + pushed * FRAME, FRAME);
            double dt = fps > 0 ? 1000.0 / fps
                                : (((pushed + 1) % (size_t)every) ? t_fast : t_slow);
            t_push += dt;
            pushed++;
        } else {
            if ((size_t)(p - dst) + FRAME > out_cap) break;
            snes_stretch_pull(p, FRAME);
            p += FRAME; pulls++;
            t_pull += t_period;
        }
    }

    size_t n = (size_t)(p - dst);
    write_wav(out, dst, n, 16000);

    double in_ms = fps > 0 ? frames * 1000.0 / fps
                           : frames * ((every - 1) * t_fast + t_slow) / every;
    printf("in   %zu frames = %.2f s emulated audio at %.2f fps\n",
           frames, frames * FRAME / 16000.0, frames * 1000.0 / in_ms);
    printf("out  %zu samples = %.2f s real time (%zu pulls)\n", n, n / 16000.0, pulls);
    printf("mode %s\n", g_snes_audio_gapfree ? "FOLLOW (gap-free, transposed)"
                                             : "PITCH (spliced)");
    printf("rate %.4fx  (playback step; 1.0 = in tune)\n", snes_stretch_step_q16() / 65536.0);
    printf("splices    %u   in %u pulls (%u judged noise)\n",
           g_stretch_ins, g_stretch_pulls, g_stretch_noise_pulls);
    printf("underruns  %u\n", snes_stretch_underruns());
    printf("wav        %s\n", out);
    return 0;
}
