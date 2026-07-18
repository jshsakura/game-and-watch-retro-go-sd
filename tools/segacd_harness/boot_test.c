/* Host Sega CD BOOT harness — iterate on the BIOS/CDD boot without the device.
 *
 *   ./boot_test <bios_CD_U.bin> <game.cue> [frames]
 *
 * Boots the MAIN 68K from the region BIOS at $000000 (not a cartridge), wires
 * the CD hardware (segacd_*), opens the disc, and runs frames — reporting how
 * far the BIOS gets (VDP writes, gate-array polls, sub-CPU release, sectors
 * read). This is the harness the boot is driven against; the device is the
 * final judge but this is where the CDD/CDC state machine gets debugged.
 *
 * Links the SAME gwenesis MD base as the device plus the segacd core; the main
 * and sub 68K share the one `m68k` global, swapped by segacd_run_sub.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "gwenesis_bus.h"
#include "gwenesis_io.h"
#include "gwenesis_vdp.h"
#include "gwenesis_sn76489.h"
#include "m68k.h"
#include "z80inst.h"
#include "ym2612.h"
#include "segacd.h"

#define VINT_H32_CYCLES 770u
#define VINT_H40_CYCLES 788u
#define SUB_CYCLES_PER_FRAME 208333u

extern unsigned char gwenesis_vdp_regs[0x20];
extern unsigned short gwenesis_vdp_status;
extern int hint_pending;
extern unsigned int screen_width, screen_height;
extern int mode_pal;

void ahb_init(void){} void itc_init(void){}
void *ahb_malloc(size_t s){return malloc(s);} void *ahb_calloc(size_t n,size_t s){return calloc(n,s);}
void *itc_malloc(size_t s){return malloc(s);} void *itc_calloc(size_t n,size_t s){return calloc(n,s);}
void *ram_malloc(size_t s){return malloc(s);} void *ram_calloc(size_t n,size_t s){return calloc(n,s);}

int system_clock, scan_line, hint_counter, skip_first_vint, drawFrame=1;
/* boot-stall (0716): count MAIN VINT raises vs. VBlank-ISR entries. The ISR at
 * BIOS 0x9a4/0x9dc is the ONLY thing that clears the $fe26 WaitVSync semaphore;
 * if the main spins at 0xa1a it's because this ISR never runs. */
long g_vint_raised=0, g_vint_taken=0; int g_reg1_at_stall=0;

/* --- budget-table A/B probes (b2 SNES methodology) ---
 * Set via getenv at startup so a single binary can run all variants. Each flag
 * stubs one component so its wall-clock cost can be measured by subtraction.
 *   SCD_SKIP_AUDIO=1 -> stub z80_run + SN76489_run + ym2612_run
 *   SCD_SKIP_VDP=1   -> stub gwenesis_vdp_render_line
 *   SCD_SKIP_SUB=1   -> stub segacd_run_sub
 *   SCD_MAIN_IDLE_SKIP=1 -> skip m68k_run slice when PC is at a spin loop
 *                           (general-purpose opcode-pattern detection, not
 *                           hardcoded PCs). Real-optimization preview.
 *   SCD_SUB_IDLE_SKIP=1  -> same for sub-68K inside segacd_run_sub's chunk loop.
 */
static int g_skip_audio, g_skip_vdp, g_skip_sub, g_main_idle_skip;
static int g_skip_ym, g_skip_psg, g_skip_z80;
static int g_main_idle_hits;
uint32_t scd_z80_idle_hits; /* referenced by z80inst.c under SCD_Z80_IDLE_SKIP */
#ifdef SCD_YM_PROBE
extern uint32_t g_ym_opcalc_total, g_ym_opcalc_skip, g_ym_samples;
extern uint32_t g_ym_chan_silent[6], g_ym_chan_total[6], g_ym_all_silent_samples;
#endif
#ifdef SCD_YM_SILENCE_SKIP
extern unsigned int g_ym_silence_skipped_samples;
#endif
/* VDP sub-component skip flags (only active when gwenesis_vdp_gfx.c is
 * compiled with -DSCD_BENCH_VDP). */
int g_vdp_skip_b, g_vdp_skip_aw, g_vdp_skip_sp;
/* General-purpose spin detector + sub idle-skip controls (defined in
 * segacd_engine.c). Main uses the same detector so both CPUs share one
 * pattern-matching codebase. */
extern int scd_m68k_is_spin(unsigned int pc);
extern int scd_sub_idle_skip;
extern uint32_t scd_sub_idle_hits;
int sn76489_clock, sn76489_index, ym2612_clock, ym2612_index, vert_screen_offset, hori_screen_offset;
unsigned int lines_per_frame = LINES_PER_FRAME_NTSC;
int16_t gwenesis_ym2612_buffer[GWENESIS_AUDIO_BUFFER_CAPACITY];
int16_t gwenesis_sn76489_buffer[GWENESIS_AUDIO_BUFFER_CAPACITY];
const unsigned char *ROM_DATA; unsigned int ROM_DATA_LENGTH;
void gwenesis_io_get_buttons(void){} void wdog_refresh(void){}

/* When SEGACD_GA_TRACE is off (build_bench.sh variant), the scd_dbg_* counters
 * are not defined anywhere. Provide weak zero/null instances so boot_test.c
 * still links without modification. The values just read as 0/empty, which is
 * correct for the un-instrumented build. */
__attribute__((weak)) uint32_t scd_dbg_prgwin_w;
__attribute__((weak)) uint8_t scd_dbg_prg_written[1];
__attribute__((weak)) uint32_t scd_dbg_wpc[1];
__attribute__((weak)) int scd_dbg_wpc_n;
__attribute__((weak)) uint32_t scd_dbg_first_a0, scd_dbg_first_a1, scd_dbg_first_ea;
__attribute__((weak)) uint32_t scd_dbg_maxpc;
__attribute__((weak)) uint32_t scd_dbg_chunks, scd_dbg_deliver4, scd_dbg_deliver2;
__attribute__((weak)) int scd_dbg_frame;

static uint16_t s_fb[320*240];
uint16_t *lcd_get_active_buffer(void){return s_fb;}
void lcd_swap(void){} void lcd_wait_for_vblank(void){} void lcd_set_refresh_rate(int h){(void)h;}
void common_emu_clear_dwt_cycles(void){} int common_emu_frame_loop(void){return 0;}
void common_ingame_overlay(void){} void common_emu_sound_sync(bool b){(void)b;}
void odroid_audio_init(int f){(void)f;} void odroid_audio_submit(int16_t*b,uint16_t n){(void)b;(void)n;}

/* The region BIOS pointer segacd_bus.c maps at main $000000 (weak in main_segacd
 * on device; here we provide it from the loaded file). */
const uint8_t *segacd_bios;

/* Wrappers so the budget probes can stub one component at a time without
 * touching call sites. MAIN_IDLE_SKIP short-circuits m68k_run when the main
 * is parked in a spin loop — detected by the general-purpose opcode-pattern
 * matcher scd_m68k_is_spin() (covers $fe26 WaitVSync, BTST polls, BRA-self,
 * etc. — not hardcoded PCs). */
static inline void run_main(uint32_t target){
    /* Probe-then-skip idle detection: run a small slice (16 cycles ≈ 2
     * spin iterations). If the main is in a spin loop, it'll still be at
     * the spin PC afterward (the probe didn't have enough work to leave
     * the loop). If an interrupt fired or real work was pending, the PC
     * will have moved elsewhere — fall through to normal execution.
     *
     * This naturally handles the int_level problem: even when a hardware
     * IRQ line is asserted (int_level > 0), if the CPU's SR mask blocks
     * it (or the handler already ran and RTE'd back), the probe will show
     * the main spinning, and we skip safely. No need to inspect int_level
     * or SR — the probe IS the check. */
    if (g_main_idle_skip && target > m68k.cycles + 16) {
        unsigned int pc_before = m68k.pc;
        unsigned int probe = m68k.cycles + 16;
        m68k_run(probe);
        if (m68k.cycles < target && scd_m68k_is_spin(m68k.pc)) {
            g_main_idle_hits++;
            m68k.cycles = target;
            return;
        }
        if (m68k.cycles >= target) return; /* probe consumed the whole slice */
    }
    m68k_run(target);
}
static inline void run_z80(uint32_t target){ if(!g_skip_audio && !g_skip_z80) z80_run(target); }
static inline void run_audio(uint32_t target_system_clock){
    if(g_skip_audio) return;
    if(!g_skip_psg) gwenesis_SN76489_run(target_system_clock);
    if(!g_skip_ym)  ym2612_run(target_system_clock);
}
static inline void run_sub(int slice){ if(!g_skip_sub) segacd_run_sub(slice); }
static inline void render_line(int line){ if(!g_skip_vdp) gwenesis_vdp_render_line(line); }

static void md_scanline_frame(void)
{
    /* one base MD frame (main 68K + Z80 + VDP), same loop as perf_test/rig_md */
    screen_height = REG1_PAL?240:224; screen_width = REG12_MODE_H40?320:256;
    lines_per_frame = mode_pal?LINES_PER_FRAME_PAL:LINES_PER_FRAME_NTSC;
    vert_screen_offset = mode_pal?0:320*(240-224)/2;
    gwenesis_vdp_set_buffer(&s_fb[vert_screen_offset]); gwenesis_vdp_render_config();
    system_clock=0; zclk=0; ym2612_clock=ym2612_index=0; sn76489_clock=sn76489_index=0; scan_line=0;
    int line;
    /* Interleave the SUB 68K per scanline, not once per whole frame. The BIOS
     * logo animation runs a tight per-frame handshake — the main VBlank ISR
     * rings the sub's level-2 doorbell ($A12000) and then SPINS waiting for the
     * sub's $A1200F ack before it grants Word-RAM (DMNA). GPGX runs both CPUs
     * lockstep per scanline, so the ack comes back mid-frame; running the sub
     * only AFTER the whole main frame (the old model) means the main never sees
     * the ack within its frame and the handshake deadlocks. Slice the sub's
     * frame budget across the ~262 scanlines so it can respond intra-frame. */
    int sub_slice = (int)(SUB_CYCLES_PER_FRAME / lines_per_frame);
    gwenesis_vdp_status=(unsigned short)((gwenesis_vdp_status&(unsigned short)~0x0112u)|STATUS_VBLANK);
    gwenesis_vdp_status^=STATUS_ODDFRAME;
    scan_line=(int)screen_height;
    if(!skip_first_vint){ gwenesis_vdp_status|=STATUS_VIRQPENDING;
      if(REG1_VBLANK_INTERRUPT){m68k_set_irq(6); g_vint_raised++;} z80_irq_line(1); }
    run_main(system_clock+VDP_CYCLES_PER_LINE); run_z80(system_clock+VDP_CYCLES_PER_LINE);
    system_clock+=VDP_CYCLES_PER_LINE; z80_irq_line(0);
    for(line=(int)screen_height+1; line<(int)lines_per_frame-1; line++){ scan_line=line;
      run_main(system_clock+VDP_CYCLES_PER_LINE); run_z80(system_clock+VDP_CYCLES_PER_LINE); system_clock+=VDP_CYCLES_PER_LINE;
      run_sub(sub_slice); }
    scan_line=(int)lines_per_frame-1; hint_counter=(int)REG10_LINE_COUNTER;
    gwenesis_vdp_status&=(unsigned short)~STATUS_VBLANK;
    run_main(system_clock+VDP_CYCLES_PER_LINE); run_z80(system_clock+VDP_CYCLES_PER_LINE); system_clock+=VDP_CYCLES_PER_LINE;
    for(line=0; line<(int)screen_height; line++){ scan_line=line; gwenesis_vdp_latch_line_scroll(line);
      if(hint_counter==0){hint_counter=(int)REG10_LINE_COUNTER; hint_pending=1; if(REG0_LINE_INTERRUPT)m68k_update_irq(4);} else hint_counter--;
      run_main(system_clock+VDP_CYCLES_PER_LINE); run_z80(system_clock+VDP_CYCLES_PER_LINE);
      render_line(line); system_clock+=VDP_CYCLES_PER_LINE;
      run_sub(sub_slice); }
    run_audio(system_clock); m68k.cycles-=system_clock;
    skip_first_vint=0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr,"usage: boot_test <bios.bin> <game.cue> [frames]\n"); return 2; }
    int FRAMES = argc > 3 ? atoi(argv[3]) : 600;

    /* budget-table probes — env-driven so a single binary runs every variant */
    g_skip_audio      = getenv("SCD_SKIP_AUDIO")      ? atoi(getenv("SCD_SKIP_AUDIO"))      : 0;
    g_skip_vdp        = getenv("SCD_SKIP_VDP")        ? atoi(getenv("SCD_SKIP_VDP"))        : 0;
    g_skip_sub        = getenv("SCD_SKIP_SUB")        ? atoi(getenv("SCD_SKIP_SUB"))        : 0;
    g_main_idle_skip  = getenv("SCD_MAIN_IDLE_SKIP")  ? atoi(getenv("SCD_MAIN_IDLE_SKIP"))  : 0;
    g_skip_ym         = getenv("SCD_SKIP_YM")         ? atoi(getenv("SCD_SKIP_YM"))         : 0;
    g_skip_psg        = getenv("SCD_SKIP_PSG")        ? atoi(getenv("SCD_SKIP_PSG"))        : 0;
    g_skip_z80        = getenv("SCD_SKIP_Z80")        ? atoi(getenv("SCD_SKIP_Z80"))        : 0;
    g_vdp_skip_b      = getenv("SCD_SKIP_PLANEB")     ? atoi(getenv("SCD_SKIP_PLANEB"))     : 0;
    g_vdp_skip_aw     = getenv("SCD_SKIP_PLANEA")     ? atoi(getenv("SCD_SKIP_PLANEA"))     : 0;
    g_vdp_skip_sp     = getenv("SCD_SKIP_SPRITES")    ? atoi(getenv("SCD_SKIP_SPRITES"))    : 0;
    /* SCD_SUB_IDLE_SKIP is intentionally NOT parsed: the $36a9 spin is a
     * bidirectional handshake (sub must process L2 ISR and write ack before
     * main re-pulses doorbell). Skipping the sub's execution prevents the
     * handshake from completing — L2 count drops to 0 and boot stalls.
     * Sub idle-skip is unsound for handshake spins; only a true hardware
     * sleep (STOP instruction, or BRA-self with no observer) could use it. */
    scd_sub_idle_skip = 0;
    /* HLE fast-boot gate injection (SEGACD_BOOT_CROSSING_RE.md §7).
     * Skips the 750-frame CDD state machine by directly injecting the
     * disc-present flag the BIOS is polling for. */
    extern int scd_fast_boot;
    scd_fast_boot = getenv("SCD_FAST_BOOT") ? atoi(getenv("SCD_FAST_BOOT")) : 0;
    int quiet = getenv("SCD_QUIET") ? atoi(getenv("SCD_QUIET")) : 0;
    if (!quiet) {
        printf("[probe] flags: audio=%d vdp=%d sub=%d main_idle=%d sub_idle=%d ym=%d psg=%d z80=%d fast_boot=%d frames=%d\n",
               g_skip_audio, g_skip_vdp, g_skip_sub, g_main_idle_skip, scd_sub_idle_skip,
               g_skip_ym, g_skip_psg, g_skip_z80, scd_fast_boot, FRAMES);
    }

    /* --- load region BIOS (becomes the main-CPU boot ROM at $000000) --- */
    FILE *bf = fopen(argv[1],"rb");
    if(!bf){ fprintf(stderr,"BIOS not found: %s (it's on the RPi5 fleet now)\n",argv[1]); return 1; }
    fseek(bf,0,SEEK_END); long bn=ftell(bf); fseek(bf,0,SEEK_SET);
    unsigned char *bios = malloc(bn); if(fread(bios,1,bn,bf)!=(size_t)bn){return 1;} fclose(bf);
    for(long i=0;i+1<bn;i+=2){ unsigned char t=bios[i]; bios[i]=bios[i+1]; bios[i+1]=t; }  /* 68K endian */
    printf("[boot] BIOS %ld bytes\n", bn);

    /* gwenesis base boots main 68K from ROM_DATA -> point it at the BIOS. */
    ROM_DATA = bios; ROM_DATA_LENGTH = (unsigned)bn;
    load_cartridge(); power_on(); reset_emulation();
    skip_first_vint=1; hint_counter=0xff;
    gwenesis_vdp_set_buffer(&s_fb[0]);

    /* --- Sega CD hardware --- */
    segacd_init();
    segacd_bios = bios;
    segacd_map_bios(segacd_bios);       /* main $000000 = BIOS (redundant w/ ROM_DATA but explicit) */
    segacd_main_map_cd_space();
    if (segacd_cd_open(argv[2]) != 0) { fprintf(stderr,"cue open failed: %s\n",argv[2]); return 1; }
    printf("[boot] disc opened, sub_running=%d\n", SCD.sub_running);

    /* --- run frames, watch boot progress --- */
    uint32_t last_fb = 0;
    int prev_running = 0;
#ifdef SEGACD_GA_TRACE
    extern int scd_dbg_frame;
    int vblank_ie_first_frame = -1;
#endif
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    /* per-ISR chunk counters: zero at the start of the timed window so the
     * dump covers exactly the bench window (matches the histogram, which also
     * starts cold-boot around frame 120). */
    extern uint32_t scd_sub_isr_chunks[8];
    for (int i = 0; i < 8; i++) scd_sub_isr_chunks[i] = 0;
    scd_z80_idle_hits = 0;
    for (int frame=0; frame<FRAMES; frame++) {
#ifdef SEGACD_GA_TRACE
        scd_dbg_frame = frame;
        if (vblank_ie_first_frame < 0 && REG1_VBLANK_INTERRUPT) vblank_ie_first_frame = frame;
#endif
        button_state[0]=0xFF;
#ifdef HOOK_CPU
        { extern int g_scd_frame; g_scd_frame = frame; }
        /* 32X-style histogram: discard the cold-boot warmup, sample from
         * frame 120 (well past the sub-release + checksum phase, where the
         * mode-8 spin loop has settled in). Schedule a lazy clear: the
         * histogram fires the actual memset on the next cpu_hook tick, so
         * we don't half-clear between main and sub work. */
        if (frame == 120) {
            extern void scd_hist_clear(void);
            scd_hist_clear();
            printf("[hist] clear scheduled at frame 120\n");
        }
#endif
        md_scanline_frame();                 /* main 68K + VDP (BIOS runs here) */

        /* HLE fast-boot per-frame injection (§7.3 gate 1 + §7.7 $FFDDC).
         * Update boot mode global + inject gates.
         * Gate 1 ($FFFE20): re-injected every frame (VBlank ISR may clear).
         * Gate 4 ($FFDDC=0x04): drive-status byte, BIOS $14be checks ==4 or ==6.
         *   Stuck at 0x03 because CDD RS1-RS8 not continuously updated (unlike PicoDrive).
         *   Inject 0x04 during mode 8 phase to unblock mode 8→0x10 crossing.
         * Stop after boot reaches mode 0x10 (LOGO = crossing success). */
        if (scd_fast_boot) {
            extern unsigned int scd_boot_mode;
            extern unsigned char *M68K_RAM;
            scd_boot_mode = (M68K_RAM[0xfdda^1]<<8)|M68K_RAM[0xfddb^1];
            if (scd_boot_mode < 0x10) {
                M68K_RAM[0xfe20^1] = 0x40;  /* $FFFE20 hi-byte = disc-present */
                /* $FFDDC drive-status: inject 0x04 (READY) during mode >= 8 to
                 * unblock BIOS $14be state-machine check. Mode 8 is where the
                 * sub stalls at $6132 because CDD status fields are frozen. */
                if (scd_boot_mode >= 8) {
                    M68K_RAM[0xfddc^1] = 0x04;
                    /* 궁극의 HLE 바이패스: mode 8 도달 시 main PC를 강제로 $064C로
                     * 세팅하여 BIOS mode8 루프 전체를 스킵. $064C는 BIOS의
                     * game-program-entry 루틴 (ROM→RAM 매핑 전환 후 IP 진입).
                     * 단 한 번만 실행 (static forced). */
                    static int forced_entry = 0;
                    if (!forced_entry) {
                        forced_entry = 1;
                        extern unsigned int m68k_get_reg(m68k_register_t reg);
                        extern void m68k_set_reg(m68k_register_t reg, unsigned int value);
                        unsigned int oldpc = m68k_get_reg(M68K_REG_PC);
                        m68k_set_reg(M68K_REG_PC, 0x064C);
                        printf("[HLE] f%d: FORCE main PC %06x -> $064C (skip BIOS mode8 loop)\n",
                               frame, oldpc);
                    }
                    /* 경로 B: HLE IP load — bypass CDC/CDD DMA entirely.
                     * Read IP (Initial Program) directly from CD image sector 0
                     * and memcpy into main RAM. BIOS mode 8 loop will JMP $064C
                     * (IP entry) once $FE20 high nibble is nonzero + $FFDDC check
                     * passes + $C100 counter expires.
                     * IP header (sector 0 user data): load addr=$0200, size=$0600.
                     * IP code starts at user data offset $100 (after 256-byte header).
                     * MODE1/2352: user data at file offset 0x10. */
                    static int ip_loaded = 0;
                    if (!ip_loaded) {
                        ip_loaded = 1;
                        /* Parse cue to find .bin path, then read IP directly.
                         * MODE1/2352: sector 0 user data at file offset 0x10. */
                        char binpath[512];
                        FILE *cf = fopen(argv[2], "r");  /* cue = text */
                        if (cf) {
                            char line[512];
                            binpath[0] = 0;
                            while (fgets(line, sizeof(line), cf)) {
                                char *p = strstr(line, "FILE ");
                                if (p) {
                                    p += 5;  /* skip "FILE " */
                                    if (*p == '"') p++;
                                    char *q = strchr(p, '"');
                                    int len = q ? (int)(q-p) : strlen(p);
                                    /* Resolve relative to cue dir */
                                    const char *slash = strrchr(argv[2], '/');
                                    if (slash) {
                                        int dlen = (int)(slash - argv[2]) + 1;
                                        memcpy(binpath, argv[2], dlen);
                                        memcpy(binpath+dlen, p, len);
                                        binpath[dlen+len] = 0;
                                    } else {
                                        memcpy(binpath, p, len);
                                        binpath[len] = 0;
                                    }
                                    break;
                                }
                            }
                            fclose(cf);
                        }
                        if (binpath[0]) {
                            FILE *bf2 = fopen(binpath, "rb");
                            if (bf2) {
                                unsigned char hdr[0x100];
                                fseek(bf2, 0x10, SEEK_SET);  /* MODE1/2352 user data */
                                fread(hdr, 1, sizeof(hdr), bf2);
                                /* IP load addr (longword at IP $40) + size (IP $44) */
                                unsigned ip_load = (hdr[0x40]<<24)|(hdr[0x41]<<16)|(hdr[0x42]<<8)|hdr[0x43];
                                unsigned ip_size = (hdr[0x44]<<24)|(hdr[0x45]<<16)|(hdr[0x46]<<8)|hdr[0x47];
                                if (ip_size > 0 && ip_size <= 0x10000 && ip_load < 0x10000) {
                                    unsigned char *ipbuf = malloc(ip_size);
                                    fseek(bf2, 0x10 + 0x100, SEEK_SET);  /* IP code after header */
                                    fread(ipbuf, 1, ip_size, bf2);
                                    for (unsigned i = 0; i+1 < ip_size; i += 2) {
                                        M68K_RAM[(ip_load + i) ^ 1] = ipbuf[i];
                                        M68K_RAM[(ip_load + i + 1) ^ 1] = ipbuf[i+1];
                                    }
                                    free(ipbuf);
                                    printf("[HLE] f%d: IP loaded from %s — addr=$%04X size=$%04X entry=$064C\n",
                                           frame, binpath, ip_load, ip_size);
                                }
                                fclose(bf2);
                            }
                        }
                    }
                }
            }
        }

        { extern unsigned char *M68K_RAM;    /* boot-mode ($FFFDDA) transition log */
          static unsigned prev_bm = 0xffff;
          unsigned bm = (M68K_RAM[0xfdda^1]<<8)|M68K_RAM[0xfddb^1];
          if (bm != prev_bm) {
              printf("[mode] f%-3d $FFFDDA %#06x -> %#06x  $FFD007=%02x $FE3A=%02x $FFFDDC=%02x $FE51=%02x $FE52=%02x $FE26=%02x\n",
                     frame, prev_bm, bm, M68K_RAM[0xd007^1], M68K_RAM[0xfe3a^1],
                     M68K_RAM[0xfddc^1], M68K_RAM[0xfe51^1], M68K_RAM[0xfe52^1],
                     M68K_RAM[0xfe26^1]);
              prev_bm = bm; } }
        if ((frame % 100) == 0) { extern unsigned char *M68K_RAM;  /* where is the main stuck? */
            unsigned c100 = (M68K_RAM[0xc101^1]<<8)|M68K_RAM[0xc100^1];
            printf("[main] f%-4d PC=%06x $C100(cnt)=%04x $FFFDDC=%02x $FE3A=%02x $FE51=%02x $FE52=%02x\n",
                   frame, (unsigned)m68k.pc, c100, M68K_RAM[0xfddc^1], M68K_RAM[0xfe3a^1],
                   M68K_RAM[0xfe51^1], M68K_RAM[0xfe52^1]); }
        if (!prev_running && SCD.sub_running) {  /* sub just released — pristine image */
            prev_running = 1;
            #define PRGW(o) ((SCD.prg_ram[((o)+1)&(SEGACD_PRG_RAM_SIZE-1)]<<8) | SCD.prg_ram[(o)&(SEGACD_PRG_RAM_SIZE-1)])
            unsigned end = (PRGW(0x1a4)<<16)|PRGW(0x1a6); uint16_t sum=0;
            for (unsigned o=0x200; o<end; o+=2) sum += PRGW(o);
            printf("[boot] >>> at sub-release f%d: checksum sum(0x200..%06x)=%04x expected=%04x %s\n",
                   frame, end, sum, PRGW(0x18e), (sum==PRGW(0x18e))?"MATCH":"MISMATCH");
            #undef PRGW
        }
        /* sub 68K now runs INTERLEAVED per-scanline inside md_scanline_frame
         * (see the sub_slice calls there) so the intra-frame main<->sub comm
         * handshake works; no longer run as one big slice here. */
        segacd_cdd_process();
        segacd_cd_update();

        /* Dense per-frame handshake trace across the animation window: main PC
         * (is it stuck at the $1288 comm spin or does it reach the $1F5E DMNA
         * grant?), sub PC, the Word-RAM mode reg ($A12003), the sub->main comm
         * flag ($A1200F=regs[0x0f]), the main->sub comm flag ($A1200E), the
         * doorbell pending flag, and IEN. */
        if (frame >= 122 && frame <= 175) {
            printf("[hs] f%-3d mainPC=%06x subPC=%06x A12003=%02x A1200F=%02x A1200E=%02x ifl2=%u ien=%02x wordnz=%s\n",
                   frame, (unsigned)m68k.pc, (unsigned)SCD.sub_ctx.pc,
                   SCD.s68k_regs[0x03], SCD.s68k_regs[0x0f], SCD.s68k_regs[0x0e],
                   (unsigned)SCD.ga_ifl2, SCD.s68k_regs[0x33],
                   "");
        }

        int sample_period = (frame > 60 && frame < 140) ? 5 : 60;
        if ((frame % sample_period) == 0) {
#ifdef SEGACD_GA_TRACE
            extern uint32_t scd_dbg_chunks, scd_dbg_deliver4, scd_dbg_deliver2;
#endif
            uint32_t h = 2166136261u; for(int k=0;k<320*240;k++) h=(h^s_fb[k])*16777619u;
            printf("[boot] f%-4d sub_running=%d subPC=%06x mainPC=%06x idle=%u cdd_status=%02x "
                   "cmd=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x ga_ifl2=%u cdd_pend=%d ien=%02x "
#ifdef SEGACD_GA_TRACE
                   "chunks=%u d4=%u d2=%u "
#endif
                   "%s\n",
                   frame, SCD.sub_running, (unsigned)SCD.sub_ctx.pc, (unsigned)m68k.pc, (unsigned)SCD.sub_idle,
                   SCD.s68k_regs[0x38 & (SEGACD_GA_REGS-1)],
                   SCD.s68k_regs[0x42],SCD.s68k_regs[0x43],SCD.s68k_regs[0x44],SCD.s68k_regs[0x45],
                   SCD.s68k_regs[0x46],SCD.s68k_regs[0x47],SCD.s68k_regs[0x48],SCD.s68k_regs[0x49],
                   SCD.s68k_regs[0x4a],SCD.s68k_regs[0x4b],
                   (unsigned)SCD.ga_ifl2, SCD.cdd_int_pending, SCD.s68k_regs[0x33],
#ifdef SEGACD_GA_TRACE
                   scd_dbg_chunks, scd_dbg_deliver4, scd_dbg_deliver2,
#endif
                   (h!=last_fb)?"(VDP active)":"");
            last_fb = h;
            /* $5e8/$5ee = this bios_CD_U.bin's sub-BIOS "wait for main's IFL2
             * doorbell" primitive (bset $5ea4 bit0; btst/bne spin; cleared
             * only by the level-2 ISR at $5f2 — see segacd/CLAUDE.md boot
             * notes). Addresses are specific to this BIOS revision; when the
             * sub parks here, print the return address off its stack so we
             * know WHICH of the several call sites (there are 5: $3fe $436
             * $4ca $50a $52e) is blocked, without re-disassembling by hand. */
            if (SCD.sub_ctx.pc == 0x5e8 || SCD.sub_ctx.pc == 0x5ee) {
                unsigned sp = SCD.sub_ctx.dar[15];
                #define PRGWx(o) ((SCD.prg_ram[((o)+1)&(SEGACD_PRG_RAM_SIZE-1)]<<8) | SCD.prg_ram[(o)&(SEGACD_PRG_RAM_SIZE-1)])
                unsigned ret = (PRGWx(sp)<<16) | PRGWx(sp+2);
                #undef PRGWx
                printf("[boot]   -> sub parked at wait-loop, SP=%06x return-addr=%06x\n", sp, ret);
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms_total = (t1.tv_sec - t0.tv_sec)*1000L + (t1.tv_nsec - t0.tv_nsec)/1000000L;
    /* Single-line BUDGET marker parsed by bench.sh. Always emitted (even in
     * SCD_QUIET) so the script can grep it cleanly. Format:
     *   BUDGET ms=<total> frames=<n> mspf=<x.xx> idle_hits=<n> */
    printf("BUDGET ms=%ld frames=%d mspf=%.2f idle_hits=%d sub_idle_hits=%lu z80_idle_hits=%lu\n",
           ms_total, FRAMES, (double)ms_total / FRAMES, g_main_idle_hits,
           (unsigned long)scd_sub_idle_hits, (unsigned long)scd_z80_idle_hits);
    /* PRG-RAM bank usage — determines device feasibility (256K vs 384K vs 512K) */
    {
        extern uint8_t scd_prg_bank_written, scd_prg_bank_accessed, scd_word_mode_seen;
#ifdef HOOK_CPU
        extern uint32_t scd_sub_max_addr, scd_sub_max_addr_frame;
        extern uint32_t scd_sub_max_prg_addr, scd_sub_max_prg_frame;
        printf("[prg_banks] written=0x%x accessed=0x%x word_mode=0x%x sub_max_addr=0x%06x@f%u sub_max_prg=0x%06x@f%u\n",
               scd_prg_bank_written, scd_prg_bank_accessed, scd_word_mode_seen,
               scd_sub_max_addr, scd_sub_max_addr_frame,
               scd_sub_max_prg_addr, scd_sub_max_prg_frame);
#else
        printf("[prg_banks] written=0x%x accessed=0x%x word_mode=0x%x (sub_max needs HOOK_CPU)\n",
               scd_prg_bank_written, scd_prg_bank_accessed, scd_word_mode_seen);
#endif
    }
    /* per-ISR chunk dump (b9 subdivision of sub-68K cost). chunks * 400 = cycles. */
    {
        extern uint32_t scd_sub_isr_chunks[8];
        uint64_t total = 0;
        for (int i = 0; i < 8; i++) total += scd_sub_isr_chunks[i];
        printf("BUDGET_ISR total=%llu", (unsigned long long)total);
        const char *names[8] = {"fg","L1gfx","L2ifl","r3","L4cdd","L5cdc","r6","r7"};
        for (int i = 0; i < 8; i++) {
            double pct = total ? 100.0 * scd_sub_isr_chunks[i] / total : 0.0;
            printf(" %s=%u(%.1f%%)", names[i], scd_sub_isr_chunks[i], pct);
        }
        printf("\n");
    }
    /* CD data feed diagnostic */
    {
        extern uint32_t scd_cdupd_pass, scd_cdupd_feed;
        printf("[cdupd] gate_pass=%u data_feed=%u\n", scd_cdupd_pass, scd_cdupd_feed);
    }
    printf("[boot] done %d frames. sub_running=%d\n", FRAMES, SCD.sub_running);
#ifdef SCD_YM_SILENCE_SKIP
    printf("[ym_silence] skipped=%u/%lu (%.1f%%)\n",
           g_ym_silence_skipped_samples,
           (unsigned long)(FRAMES * (GWENESIS_AUDIO_BUFFER_CAPACITY > 0 ? 889 : 889)),
           (FRAMES > 0) ? 100.0 * g_ym_silence_skipped_samples / (unsigned long)(FRAMES * 889) : 0.0);
#endif
#ifdef SCD_YM_PROBE
    printf("[ym_probe] samples=%lu opcalc_total=%lu opcalc_skip=%lu skip_rate=%.1f%%\n",
           (unsigned long)g_ym_samples,
           (unsigned long)g_ym_opcalc_total,
           (unsigned long)g_ym_opcalc_skip,
           g_ym_opcalc_total ? 100.0 * g_ym_opcalc_skip / g_ym_opcalc_total : 0.0);
    printf("[ym_chan] all_silent=%lu/%lu (%.1f%%)\n",
           (unsigned long)g_ym_all_silent_samples,
           (unsigned long)g_ym_samples,
           g_ym_samples ? 100.0 * g_ym_all_silent_samples / g_ym_samples : 0.0);
    for (int i = 0; i < 6; i++)
        printf("  ch%d: silent=%lu/%lu (%.1f%%)\n", i,
               (unsigned long)g_ym_chan_silent[i],
               (unsigned long)g_ym_chan_total[i],
               g_ym_chan_total[i] ? 100.0 * g_ym_chan_silent[i] / g_ym_chan_total[i] : 0.0);
#endif
#ifdef HOOK_CPU
    /* dump top-20 opcode + top-20 PC for each 68K — same shape as the 32X
     * Phase-1.7 histogram (memory sega32x-feasibility.md). Output is what we
     * build the budget table from. */
    { extern void scd_hist_dump(void); scd_hist_dump(); }
#endif
    /* boot-stall probe: is MAIN's VBlank interrupt actually being raised, and
     * is the main CPU able to take it (SR mask), at the moment we stopped? */
    printf("[boot] MAIN VINT raised=%ld times; VDP reg1=%02x (VINT-enable bit5=%d, disp-en bit6=%d); "
           "main SR int_mask=%u pc=%06x int_level=%u\n",
           g_vint_raised, gwenesis_vdp_regs[1], (gwenesis_vdp_regs[1]>>5)&1,
           (gwenesis_vdp_regs[1]>>6)&1, (unsigned)m68k.int_mask, (unsigned)m68k.pc,
           (unsigned)m68k.int_level);
    /* Dump the MAIN stack so we can see WHICH caller of WaitVSync (0xa0c) the
     * main is parked in — the return address sits at the top of its stack while
     * it spins inside WaitVSync. main RAM is M68K_RAM[addr^1] (BE pair-swap). */
    { extern unsigned char *M68K_RAM;
      unsigned sp = m68k.dar[15] & 0xffff;
      #define MRW(o) ((M68K_RAM[((o)+1)^1]<<8)|M68K_RAM[(o)^1])
      #define MRL(o) ((MRW(o)<<16)|MRW((o)+2))
      printf("[boot] MAIN SP=%06x stack longs:", m68k.dar[15]);
      for (int i=0;i<10;i++) printf(" %06x", MRL((sp+i*4)&0xffff));
      printf("\n");
      #undef MRW
      #undef MRL
    }
    /* did the main copy a sub program into PRG-RAM, and is the sub past reset? */
    { long nz=0; for(int i=0;i<SEGACD_PRG_RAM_SIZE;i++) if(SCD.prg_ram[i]) nz++;
      long wz=0; for(int i=0;i<SEGACD_WORD_RAM_SIZE;i++) if(SCD.word_ram[i]) wz++;
      printf("[boot] PRG-RAM %ld/%d nz, Word-RAM %ld/%d nz, sub PC=%06x $A12003=%02x stopped=%u int_mask=%u\n",
             nz, SEGACD_PRG_RAM_SIZE, wz, SEGACD_WORD_RAM_SIZE,
             (unsigned)SCD.sub_ctx.pc, SCD.s68k_regs[0x03],
             (unsigned)SCD.sub_ctx.stopped, (unsigned)SCD.sub_ctx.int_mask);
      unsigned pc = SCD.sub_ctx.pc & (SEGACD_PRG_RAM_SIZE-1);
      printf("[boot] sub code @PC:");
      for (int i=0;i<16;i++) printf(" %02x", SCD.prg_ram[(pc+i)&(SEGACD_PRG_RAM_SIZE-1)]);
      printf("  (IEN $FF8033=%02x)\n", SCD.s68k_regs[0x33]);
      printf("[boot] PRG reset vec (raw bytes 0..7):");
      for (int i=0;i<8;i++) printf(" %02x", SCD.prg_ram[i]);
      extern uint32_t scd_dbg_prgwin_w;
      printf("   main->PRGwin writes=%u\n", scd_dbg_prgwin_w);
      printf("[boot] sub regs D0-D7:");
      for (int i=0;i<8;i++) printf(" %08x", SCD.sub_ctx.dar[i]);
      printf("\n[boot] sub regs A0-A7:");
      for (int i=8;i<16;i++) printf(" %08x", SCD.sub_ctx.dar[i]);
      printf("\n[boot] sub int_mask=%u  A1+0x8e=%06x\n",
             (unsigned)SCD.sub_ctx.int_mask,
             (unsigned)(SCD.sub_ctx.dar[9] + 0x8e));
      /* PRG is stored byte-swapped (sub reads via direct base); reconstruct the
       * big-endian instruction/data words the sub actually sees. */
      #define PRGW(o) ((SCD.prg_ram[((o)+1)&(SEGACD_PRG_RAM_SIZE-1)]<<8) | SCD.prg_ram[(o)&(SEGACD_PRG_RAM_SIZE-1)])
      printf("[boot] sub code words @0x2c0-0x300 (BE):");
      for (unsigned o=0x2c0;o<0x300;o+=2) printf(" %04x", PRGW(o));
      printf("\n[boot] PRG comm words @0x180-0x1a0 (BE):");
      for (unsigned o=0x180;o<0x1a0;o+=2) printf(" %04x", PRGW(o));
      printf("\n[boot] poll target word @0x18e (BE)=%04x\n", PRGW(0x18e));
      unsigned len_lo = PRGW(0x1a4), len_hi = PRGW(0x1a6);
      printf("[boot] hdr sig@0x100 (BE)=%04x%04x  len@0x1a4 (BE.L)=%04x%04x\n",
             PRGW(0x100), PRGW(0x102), len_lo, len_hi);
      /* Recompute the sub-BIOS self-checksum exactly as the routine does:
       * sum BE words from 0x202 up to end=(*0x1a4), compare to word @0x18e. */
      { unsigned end = (len_lo<<16)|len_hi; uint16_t sum=0;
        for (unsigned o=0x200; o<end; o+=2) sum += PRGW(o);
        printf("[boot] checksum sum(0x200..%06x)=%04x  expected@0x18e=%04x  %s\n",
               end, sum, PRGW(0x18e), (sum==PRGW(0x18e))?"MATCH":"MISMATCH");
        /* Dump what the sub sees (BE-reconstructed) for 0x0..0x10000 so we can
         * diff against the sub-BIOS source embedded in the region BIOS image.
         * Wider than the checksummed region (0x5800): interrupt vectors/handlers
         * and CDD command tables live past the checksum window. */
        FILE *d=fopen("/tmp/scd/prg_subbios.bin","wb");
        if(d){ for(unsigned o=0x0;o<0x20000;o+=2){ uint16_t w=PRGW(o);
                 unsigned char be[2]={(unsigned char)(w>>8),(unsigned char)(w&0xff)};
                 fwrite(be,1,2,d);} fclose(d);
               printf("[boot] wrote /tmp/scd/prg_subbios.bin (0x0..0x20000 BE)\n"); }
        /* Was every byte of the checksummed region written by the main PRG window,
         * or did non-zero bytes arrive via some path our ^1 fix doesn't cover? */
        extern uint8_t scd_dbg_prg_written[];
        long covered=0, nz_uncovered=0;
        for (unsigned o=0x202; o<0x5800; o++) {
            if (scd_dbg_prg_written[o^1]) covered++;
            else if (SCD.prg_ram[o]) nz_uncovered++;
        }
        printf("[boot] checksum-region write coverage: %ld/%d bytes written via PRG-window, "
               "%ld non-zero bytes NEVER written by it\n", covered, 0x5800-0x202, nz_uncovered);
        extern uint32_t scd_dbg_wpc[]; extern int scd_dbg_wpc_n;
        printf("[boot] distinct main PCs that store into PRG (%d): ", scd_dbg_wpc_n);
        for (int i=0;i<scd_dbg_wpc_n;i++) printf("%06x ", scd_dbg_wpc[i]);
        printf("\n");
        extern uint32_t scd_dbg_first_a0, scd_dbg_first_a1, scd_dbg_first_ea;
        printf("[boot] decompressor first store: A0(src)=%06x A1(dst)=%06x storeEA=%06x\n",
               scd_dbg_first_a0, scd_dbg_first_a1, scd_dbg_first_ea);
        extern uint32_t scd_dbg_maxpc;
        printf("[boot] sub-BIOS self-checksum PASSES (0x200..0x5800 == 0xe9bb). furthest sub PC=%06x\n",
               scd_dbg_maxpc);
        /* Dump the compressed source region from the BIOS (logical BE bytes the
         * main reads) so we can decompress it independently in Python. A0 points
         * a few bytes past the start after the first control word+byte. */
        { FILE *c=fopen("/tmp/scd/compressed_src.bin","wb");
          if(c){ for(unsigned o=(scd_dbg_first_a0-0x40)&~1u; o<(scd_dbg_first_a0+0x4000); o++)
                    { unsigned char b = bios[(o^1) & (ROM_DATA_LENGTH-1)]; fwrite(&b,1,1,c);}
                 fclose(c);
                 printf("[boot] wrote /tmp/scd/compressed_src.bin from BIOS @%06x\n",
                        (scd_dbg_first_a0-0x40)&~1u); } } }
      #undef PRGW
    }

#ifdef SEGACD_GA_TRACE
    extern uint32_t scd_ga_rd[], scd_ga_wr[], scd_sga_rd[], scd_sga_wr[];
    printf("[boot] MAIN gate-array access (reg: rd/wr):\n");
    for (int r = 0; r < SEGACD_GA_REGS; r++)
        if (scd_ga_rd[r] || scd_ga_wr[r])
            printf("  $A120%02x  rd=%-8u wr=%-8u\n", r, scd_ga_rd[r], scd_ga_wr[r]);
    printf("[boot] SUB gate-array access ($FF80xx, reg: rd/wr) — what the sub-BIOS wants:\n");
    for (int r = 0; r < SEGACD_GA_REGS; r++)
        if (scd_sga_rd[r] || scd_sga_wr[r])
            printf("  $FF80%02x  rd=%-8u wr=%-8u\n", r, scd_sga_rd[r], scd_sga_wr[r]);
    extern uint32_t scd_dbg_cdd_cmd_hist[];
    printf("[boot] CDD command histogram (nibble: count):\n");
    for (int i = 0; i < 16; i++)
        if (scd_dbg_cdd_cmd_hist[i])
            printf("  cmd 0x%x: %u\n", i, scd_dbg_cdd_cmd_hist[i]);

    extern uint32_t scd_dbg_800f_pc[]; extern uint8_t scd_dbg_800f_val[]; extern int scd_dbg_800f_n;
    printf("[boot] sub writes to $FF800E/F comm-flag (%d logged): ", scd_dbg_800f_n);
    for (int i = 0; i < scd_dbg_800f_n; i++)
        printf("[pc=%06x val=%02x] ", scd_dbg_800f_pc[i], scd_dbg_800f_val[i]);
    printf("\n");
    extern uint32_t scd_dbg_a1200e_pc[]; extern uint8_t scd_dbg_a1200e_val[]; extern int scd_dbg_a1200e_n;
    printf("[boot] main writes to $A1200E/F comm-flag (%d logged): ", scd_dbg_a1200e_n);
    for (int i = 0; i < scd_dbg_a1200e_n; i++)
        printf("[pc=%06x val=%02x] ", scd_dbg_a1200e_pc[i], scd_dbg_a1200e_val[i]);
    printf("\n");
    extern uint8_t scd_dbg_a12000_regef[][2]; extern int scd_dbg_a12000_regef_n;
    printf("[boot] regs[0x0e]/[0x0f] at each $A12000 doorbell write (%d logged): ", scd_dbg_a12000_regef_n);
    for (int i = 0; i < scd_dbg_a12000_regef_n; i++)
        printf("[e=%02x f=%02x] ", scd_dbg_a12000_regef[i][0], scd_dbg_a12000_regef[i][1]);
    printf("\n");
    /* regs[0x01] here is now only ever SUB's own $FF8001 (LED/soft-reset
     * trigger) writes — main_busreq is MAIN's separate SRES/SBRQ shadow,
     * see segacd.h. Printing both makes the post-fix split visible. */
    printf("[boot] final regs[0x0e]=%02x regs[0x0f]=%02x regs[0x01](sub)=%02x main_busreq=%02x\n",
           SCD.s68k_regs[0x0e], SCD.s68k_regs[0x0f], SCD.s68k_regs[0x01], SCD.main_busreq);
    extern uint32_t scd_dbg_reg1_pc[]; extern uint8_t scd_dbg_reg1_val[]; extern uint32_t scd_dbg_reg1_frame[]; extern int scd_dbg_reg1_n;
    printf("[boot] $A12001 (SRES/SBRQ) writes (%d logged): ", scd_dbg_reg1_n);
    for (int i = 0; i < scd_dbg_reg1_n; i++)
        printf("[f%u pc=%06x val=%02x] ", scd_dbg_reg1_frame[i], scd_dbg_reg1_pc[i], scd_dbg_reg1_val[i]);
    printf("\n");

    /* Sub work-RAM CDD/TOC state machine variables ($5800-$587F), the ones
     * decoded by disassembling the level-4 (CDD) ISR chain at PRG 0x610. */
    printf("[boot] sub work-RAM CDD state ($5800-$587f):");
    for (unsigned o = 0x5800; o < 0x5880; o++) {
        printf(" %02x", SCD.prg_ram[o ^ 1]);
        if ((o & 0xf) == 0xf) printf("\n  ");
    }
    printf("\n");

    /* --- boot-stall investigation (0716): who pulses $A12000, is MAIN's
     * VBlank interrupt-enable ever set, and where was the sub parked at each
     * IFL2/CDD delivery. --- */
    printf("[boot] REG1 VBLANK_INTERRUPT (IE0) first seen enabled at frame=%d (-1=never)\n",
           vblank_ie_first_frame);
    { extern uint32_t scd_dbg_a10003_reads; extern uint8_t scd_dbg_a10003_last;
      extern unsigned char *M68K_RAM;
      unsigned bootmode = (M68K_RAM[0xfdda^1]<<8)|M68K_RAM[0xfddb^1];
      unsigned fe20 = M68K_RAM[0xfe20^1];
      printf("[boot] BOOT-MODE $FFFDDA=%#06x (4=disc-detect 8=? 0x10=LOGO)  $FFFE20=%#04x (&0xf0=%#04x)  "
             "$A10003 reads=%u last=%#04x %s\n",
             bootmode, fe20, fe20&0xf0, scd_dbg_a10003_reads, scd_dbg_a10003_last,
             (fe20&0xf0)?"(gate PASSES)":"(gate BLOCKS boot-mode advance)"); }
    /* --- DRIVE-STATUS relay chain (0718): sub writes CDD/drive status to its
     * comm regs $FF8020-2F -> main comm handler copies to $FDF0-FF -> $FE3A
     * (disc-ready == 0x40 hi-byte, gate 0x1d34) & $FFFDDC (drive-busy bit7,
     * gate 0x1d66). If this chain is empty the main never leaves disc-detect. */
    { extern unsigned char *M68K_RAM;
      printf("[boot] DRIVE-STATUS chain:\n  sub-comm $FF8020-2F(=s68k_regs):");
      for (int i = 0x20; i < 0x30; i++) printf(" %02x", SCD.s68k_regs[i]);
      printf("\n  main copy $FDF0-FF          :");
      for (unsigned o = 0xfdf0; o < 0xfe00; o++) printf(" %02x", M68K_RAM[o ^ 1]);
      printf("\n  $FE3A=%02x%02x (disc-ready hi==0x40?)  $FE51=%02x $FE52=%02x  $FFFDDC=%02x (busy bit7)"
             "  $FDDE=%02x $FDDF=%02x\n",
             M68K_RAM[0xfe3a ^ 1], M68K_RAM[0xfe3b ^ 1], M68K_RAM[0xfe51 ^ 1],
             M68K_RAM[0xfe52 ^ 1], M68K_RAM[0xfddc ^ 1], M68K_RAM[0xfdde ^ 1],
             M68K_RAM[0xfddf ^ 1]);
      /* --- mode-8 CROSSING chain (0718, decoded from BIOS 0x4bd0/0x2e2e/0x3bae):
       * sub writes a disc-status block to Word-RAM $200400; the $A12003 DMNA/RET
       * swap hands it to main; 0x4bd0 copies WordRAM $200400/402/404 -> $FFD01C/
       * 1E/20; 0x2e2e turns a nonzero $D01C into $D04A bit1/2; the $D000 state
       * machine then climbs to 0x3bae which sets $D007 bit7 -> mode 0x10. The
       * dead link (ours AND picodrive): $FFD01C stays 0. Show the whole chain. */
      #define WRAM(o) ((SCD.word_ram[((o)+1)^1]<<8)|SCD.word_ram[(o)^1])
      printf("  CROSSING chain: WordRAM $200400=%04x $200402=%04x $200404=%04x  ->"
             "  $D01C=%02x%02x $D01E=%02x%02x $D020=%02x%02x  |  $D000(state)=%02x%02x"
             "  $D04A=%02x%02x  $D007=%02x  A12003(DMNA/RET)=%02x\n",
             WRAM(0x400), WRAM(0x402), WRAM(0x404),
             M68K_RAM[0xd01c^1], M68K_RAM[0xd01d^1], M68K_RAM[0xd01e^1], M68K_RAM[0xd01f^1],
             M68K_RAM[0xd020^1], M68K_RAM[0xd021^1], M68K_RAM[0xd000^1], M68K_RAM[0xd001^1],
             M68K_RAM[0xd04a^1], M68K_RAM[0xd04b^1], M68K_RAM[0xd007^1], SCD.s68k_regs[0x03]);
      #undef WRAM
    }
    { extern uint32_t scd_dbg_mainstamp_hits;
      printf("[boot] MAIN stamp-draw (0x5f00-0x6e00) sub-slice hits=%u  %s\n",
             scd_dbg_mainstamp_hits,
             scd_dbg_mainstamp_hits ? "(main draws stamps)" : "(MAIN NEVER DRAWS STAMPS -> Word-RAM stamps empty)"); }
    { extern uint32_t scd_dbg_state8_hits, scd_dbg_stampwr_hits;
      printf("[boot] boot-logo state machine: STATE-8(stamp loader 0x7136) sub-slice hits=%u  stamp-draw hits=%u  %s\n",
             scd_dbg_state8_hits, scd_dbg_stampwr_hits,
             scd_dbg_state8_hits ? "(state 8 RAN)" : "(STATE 8 NEVER ENTERED -> stamps never loaded)"); }
    { extern uint32_t scd_dbg_gfx_ops, scd_dbg_gfx_lines, scd_dbg_gfx_mapnz;
      printf("[boot] GFX ASIC: start-ops=%u lines-rendered=%u  stamp-map max-nonzero(of 512)=%u %s\n",
             scd_dbg_gfx_ops, scd_dbg_gfx_lines, scd_dbg_gfx_mapnz,
             scd_dbg_gfx_mapnz ? "(stamps loaded)" : "(STAMP MAP EMPTY -> render is blank)"); }
    /* CDC data-path: where does the disc data get stuck on the way to Word-RAM? */
    { extern uint32_t scd_dbg_dec_calls, scd_dbg_dec_wrrq, scd_dbg_cdupd_read,
                      scd_dbg_host_sub, scd_dbg_host_sub_adv, scd_dbg_host_main,
                      scd_dbg_dma_sector;
      extern uint16_t segacd_cdc_ctrl_dbg(int which);
      extern uint32_t scd_dbg_ctrl0_w, scd_dbg_ctrl0_wrrq;
      printf("[boot] CDC CTRL0 writes by sub=%u, of which set WRRQ=%u\n", scd_dbg_ctrl0_w, scd_dbg_ctrl0_wrrq);
      printf("[boot] CDC data-path: cd_update sector-reads=%u  decoder(DECEN)=%u  ring-writes(WRRQ)=%u  "
             "|  host-port sub=%u (advanced=%u) main=%u  |  DMA-stub calls=%u\n",
             scd_dbg_cdupd_read, scd_dbg_dec_calls, scd_dbg_dec_wrrq,
             scd_dbg_host_sub, scd_dbg_host_sub_adv, scd_dbg_host_main, scd_dbg_dma_sector);
      printf("[boot] CDC state: ctrl0=%02x(DECEN=%d WRRQ=%d) ctrl1=%02x ifctrl=%02x ifstat=%02x  "
             "CD.status=%u cur_lba=%u  reg[4](DTRG/dir)=%02x\n",
             segacd_cdc_ctrl_dbg(0), (segacd_cdc_ctrl_dbg(0)>>7)&1, (segacd_cdc_ctrl_dbg(0)>>2)&1,
             segacd_cdc_ctrl_dbg(1), segacd_cdc_ctrl_dbg(2), segacd_cdc_ctrl_dbg(3),
             segacd_cdc_ctrl_dbg(4), segacd_cdc_ctrl_dbg(5), SCD.s68k_regs[0x04]);
    }
    extern uint32_t scd_dbg_a12000_frame[], scd_dbg_a12000_pc[]; extern int scd_dbg_a12000_n;
    printf("[boot] $A12000 doorbell writes (%d logged, total wr=%u): ", scd_dbg_a12000_n, scd_ga_wr[0]);
    for (int i = 0; i < scd_dbg_a12000_n; i++)
        printf("[f%u pc=%06x] ", scd_dbg_a12000_frame[i], scd_dbg_a12000_pc[i]);
    printf("\n");
    extern uint32_t scd_dbg_deliver2_frame[], scd_dbg_deliver2_pc[]; extern int scd_dbg_deliver2_n;
    printf("[boot] level-2 (IFL2) delivered to SUB (%d logged): ", scd_dbg_deliver2_n);
    for (int i = 0; i < scd_dbg_deliver2_n; i++)
        printf("[f%u subPC=%06x] ", scd_dbg_deliver2_frame[i], scd_dbg_deliver2_pc[i]);
    printf("\n");
    extern uint32_t scd_dbg_deliver4_frame[], scd_dbg_deliver4_pc[]; extern int scd_dbg_deliver4_n;
    printf("[boot] level-4 (CDD) delivered to SUB (%d logged): ", scd_dbg_deliver4_n);
    for (int i = 0; i < scd_dbg_deliver4_n; i++)
        printf("[f%u subPC=%06x] ", scd_dbg_deliver4_frame[i], scd_dbg_deliver4_pc[i]);
    printf("\n");
    extern uint32_t scd_dbg_deliver5_total, scd_dbg_deliver5_frame[], scd_dbg_deliver5_pc[]; extern int scd_dbg_deliver5_n;
    printf("[boot] level-5 (CDC/DECI) delivered to SUB total=%u (%d logged): ", scd_dbg_deliver5_total, scd_dbg_deliver5_n);
    for (int i = 0; i < scd_dbg_deliver5_n; i++)
        printf("[f%u subPC=%06x] ", scd_dbg_deliver5_frame[i], scd_dbg_deliver5_pc[i]);
    printf("\n");

    /* Dump MAIN's work RAM (BE-reconstructed, same ^1 pair-swap convention as
     * PRG-RAM — see segacd_bus.c's byte-order comment) so the installed
     * exception vector handlers (copied into RAM by the BIOS at boot; the
     * vector table in ROM points into $FFxxxx, not ROM) can be disassembled
     * offline. */
    { extern unsigned char *M68K_RAM;
      FILE *r = fopen("/tmp/scd/main_ram.bin", "wb");
      if (r) { for (unsigned o = 0; o < 0x10000; o += 2) {
                  /* M68K_RAM[addr^1] holds the logical big-endian byte at
                   * `addr` (same ^1 pair-swap READ_BYTE/WRITE_BYTE convention
                   * as PRG-RAM — verified against main_prgwin_write8/PRGW). */
                  unsigned char be[2] = { M68K_RAM[o^1], M68K_RAM[(o+1)^1] };
                  fwrite(be, 1, 2, r); }
               fclose(r);
                printf("[boot] wrote /tmp/scd/main_ram.bin (0x0..0x10000 BE, addr base $FF0000)\n"); } }

#endif
    /* Dump Z80 RAM (8KB) for offline idle-loop analysis */
    { extern unsigned char *Z80_RAM[];
      FILE *z = fopen("/tmp/scd/z80_ram.bin", "wb");
      if (z) { fwrite(Z80_RAM[0], 1, 0x2000, z);
               fclose(z);
               printf("[boot] wrote /tmp/scd/z80_ram.bin (0x0..0x2000)\n"); } }
    return 0;
}
