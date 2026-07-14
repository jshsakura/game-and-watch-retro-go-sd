/* The prover's runner: a real ROM, the real core, driven for N frames.
 *
 * It is deliberately the same shape as the firmware's frame loop — load the cart
 * the way the device loads it, hand the core an XIP ROM pointer, run
 * execute_arm(execute_cycles) once per frame, drain the audio — because a
 * harness that runs a different loop measures a different program.
 *
 * With M4A_HLE_VERIFY it does no measuring at all: m4a_gpsp.c has already run
 * every block twice and compared them, and this just prints the tally and fails
 * loudly if the hook never fired (a test that never ran is not a test).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;

void init_main(void);
void init_memory(void);
void init_sound(void);
void init_gamepak_buffer(void);
void gba_set_xip_rom(u8 *base, u32 size);
void reset_gba(void);
void execute_arm(u32 cycles);
u32  load_gamepak(const void *info, const char *name, int rtc, int rumble, int serial);
void gba_set_keys(u32 keys);
u32  sound_read_samples(int16_t *out, u32 frames);

extern u8  bios_rom[1024 * 16];
extern u8  gamepak_backup[1024 * 128];
extern u32 idle_loop_target_pc;
extern u32 execute_cycles;
extern u16 *gba_screen_pixels;

#ifdef GBA_M4A_HLE
extern unsigned int m4a_hook_pc;
const char *m4a_hle_variant_name(void);
#endif
#ifdef M4A_HLE_VERIFY
void m4a_hle_verify_report(void);
#endif

#ifdef M4A_HASH
/* The definitive test.
 *
 * The per-block verifier can prove the block's ARITHMETIC — same registers, same
 * memory, same guest cycles — and it does. What it cannot prove is the one thing
 * the native block genuinely does differently: it runs the whole block at once,
 * where the interpreter is interrupted several times along the way (the block is
 * thousands of cycles and a slice is a scanline at most). The hardware is handed
 * exactly the same cycles either way, but an interrupt that used to land in the
 * MIDDLE of the mixer now lands just after it.
 *
 * Whether that matters is not a question anyone should answer by reasoning about
 * it. So: run the same ROM twice, once with the hook and once without, and hash
 * everything the guest can see, every frame. If the two streams are identical,
 * the difference is not observable — by the game, by the screen, or by the ear.
 */
extern unsigned char *memory_map_read[8 * 1024];
extern unsigned short palette_ram[512];
extern unsigned short oam_ram[512];
extern unsigned short io_registers[512];
extern unsigned int   cpu_ticks;

static u64 fnv(u64 h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* Read a guest region through the core's own page map, so this sees exactly what
 * the emulated CPU sees and does not have to know how gpSP lays its arrays out. */
static u64 fnv_guest(u64 h, u32 addr, u32 len)
{
    while (len) {
        u32 off  = addr & 0x7FFFu;
        u32 n    = 0x8000u - off;
        unsigned char *page = memory_map_read[addr >> 15];
        if (n > len) n = len;
        if (page) h = fnv(h, page + off, n);
        addr += n;
        len  -= n;
    }
    return h;
}

/* One hash per thing, not one hash for everything: when a run diverges, "frame
 * 755 differs" is a fact you cannot act on, and "the CLOCK differs at 755 but the
 * screen does not until 902" tells you which end to pull. */
#define FNV0 1469598103934665603ull
static void frame_hashes(const u16 *fb, const int16_t *audio, u32 nframes, u64 h[8])
{
    h[0] = (u64)cpu_ticks;   /* the clock, raw: a hash of it would hide the size of the drift */
    h[1] = fnv(FNV0, io_registers, sizeof io_registers);   /* the hardware */
    h[2] = fnv_guest(FNV0, 0x03000000u, 0x8000u);          /* IWRAM */
    h[3] = fnv_guest(FNV0, 0x02000000u, 0x40000u);         /* EWRAM */
    h[4] = fnv_guest(FNV0, 0x06000000u, 0x18000u);         /* VRAM */
    h[5] = fnv(fnv(FNV0, palette_ram, sizeof palette_ram),
               oam_ram, sizeof oam_ram);                   /* palette + OAM */
    h[6] = fnv(FNV0, fb, 240u * 160u * sizeof(u16));       /* what is seen */
    h[7] = fnv(FNV0, audio, nframes * 2u * sizeof(int16_t)); /* what is heard */
}
static const char *const HNAME[8] = {
    "clock", "io", "iwram", "ewram", "vram", "pal+oam", "screen", "audio"
};
#endif

#define FEAT_AUTODETECT (-1)

/* gba_frontend.c routes the cart scan's yield to the firmware watchdog. */
void wdog_refresh(void) {}

/* Stop the clock.
 *
 * gpSP's RTC takes its baseline from the host's wall clock (gba_memory.c,
 * rtc_init_base_time -> time()), and a cart with an RTC — Pokemon Ruby, Sapphire,
 * EMERALD, Boktai — reads that straight into its own memory in the first seconds
 * of boot. So two runs of the SAME BINARY produce different IWRAM, and a
 * comparison of hook-off against hook-on is comparing two different afternoons.
 *
 * That is exactly what happened here: the stereo variant "failed" the end-to-end
 * proof at frame 12 with every other region — screen, audio, EWRAM, the clock
 * itself — bit-identical. It was not the mixer. It was Tuesday.
 *
 * Overriding time() in the harness is enough: gba_memory.o's call resolves here,
 * and the whole run becomes a function of the ROM alone. It is also honest about
 * what is being tested, which is the mixer and not the calendar. */
time_t time(time_t *t)
{
    const time_t frozen = 1700000000;   /* an arbitrary, fixed instant */
    if (t)
        *t = frozen;
    return frozen;
}

#ifdef M4A_COUNT_INSNS
unsigned long long m4a_insns_interpreted;
#endif

#define KEY_A      0x0001
#define KEY_START  0x0008
#define KEY_LEFT   0x0020

static u16 framebuffer[240 * 160];

/* Blind navigation: mash A to advance dialogue, tap START for title screens, and
 * nudge LEFT so yes/no prompts do not park on "no" for ever. It does not matter
 * where the game ends up — every frame it runs is a frame of real M4A mixing,
 * and the verifier checks every block of it. */
static u32 scripted_keys(int f)
{
    int t;
    if (f < 240) return 0;
    t = f & 127;
    if (t < 2)                  return KEY_START;
    if (t >= 6 && t < 8)        return KEY_LEFT;
    if ((f & 15) < 2 && t >= 8) return KEY_A;
    return 0;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    const char *rompath;
    int frames, f;
    long romsz;
    FILE *rf, *bf;
    u8 *rom;
    static int16_t audio[804 * 2];
    double t0, t1;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <rom.gba> [frames]\n", argv[0]);
        return 2;
    }
    rompath = argv[1];
    frames  = argc > 2 ? atoi(argv[2]) : 4000;

    rf = fopen(rompath, "rb");
    if (!rf) { perror("rom"); return 1; }
    fseek(rf, 0, SEEK_END);
    romsz = ftell(rf);
    fseek(rf, 0, SEEK_SET);
    rom = (u8 *)malloc(romsz);
    if (!rom || fread(rom, 1, romsz, rf) != (size_t)romsz) { perror("read"); return 1; }
    fclose(rf);

    bf = fopen("external/gpsp/bios/open_gba_bios.bin", "rb");
    if (!bf) { perror("bios (run from the repo root)"); return 1; }
    if (fread(bios_rom, 1, sizeof bios_rom, bf) != sizeof bios_rom) { perror("bios"); return 1; }
    fclose(bf);

    init_main();
    init_memory();
    init_sound();
    gba_set_xip_rom(rom, (u32)romsz);
    init_gamepak_buffer();
    memset(gamepak_backup, 0xFF, sizeof gamepak_backup);

    if (load_gamepak(NULL, rompath, FEAT_AUTODETECT, 0, 0) != 0) {
        printf("FAIL: load_gamepak rejected the cart\n");
        return 1;
    }
    fprintf(stderr, "rom %.4s, %ld bytes\n", (char *)&rom[0xAC], romsz);

    gba_screen_pixels = framebuffer;
    reset_gba();

    t0 = now_sec();
    for (f = 0; f < frames; f++) {
        gba_set_keys(scripted_keys(f));
        execute_arm(execute_cycles);
        sound_read_samples(audio, 804);
#ifdef M4A_HASH
        {
            const char *df = getenv("M4A_DUMP_FRAME");
            if (df && atoi(df) == f) {
                const char *dp = getenv("M4A_DUMP_PATH");
                FILE *o = fopen(dp ? dp : "/tmp/iwram_dump.bin", "wb");
                if (o) {
                    u32 a;
                    for (a = 0x03000000u; a < 0x03008000u; a += 0x8000u) {
                        unsigned char *pg = memory_map_read[a >> 15];
                        if (pg) fwrite(pg, 1, 0x8000, o);
                    }
                    fclose(o);
                }
            }
        }
        {
            u64 h[8];
            int k;
            frame_hashes(framebuffer, audio, 804, h);
            printf("%06d", f);
            for (k = 0; k < 8; k++)
                printf(" %s=%016llx", HNAME[k], (unsigned long long)h[k]);
            printf("\n");
        }
#endif
    }
    t1 = now_sec();

    fprintf(stderr, "%d frames in %.2f s  (%.0f fps on this host)\n",
            frames, t1 - t0, frames / (t1 - t0));
#ifdef M4A_COUNT_INSNS
    fprintf(stderr, "guest instructions still interpreted: %llu  (%.0f per frame)\n",
            m4a_insns_interpreted, (double)m4a_insns_interpreted / frames);
#endif

#ifdef GBA_M4A_HLE
    if (m4a_hook_pc)
        fprintf(stderr, "M4A: %s hooked at %08x\n", m4a_hle_variant_name(), m4a_hook_pc);
    else
        fprintf(stderr, "M4A: no known mixer found in this game — nothing was hooked\n");
#else
    fprintf(stderr, "M4A: hook not built in (the interpreter did all of it)\n");
#endif

#ifdef M4A_HLE_VERIFY
    m4a_hle_verify_report();
#endif
    return 0;
}
