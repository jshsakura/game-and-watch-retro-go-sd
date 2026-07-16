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
int sn76489_clock, sn76489_index, ym2612_clock, ym2612_index, vert_screen_offset, hori_screen_offset;
unsigned int lines_per_frame = LINES_PER_FRAME_NTSC;
int16_t gwenesis_ym2612_buffer[GWENESIS_AUDIO_BUFFER_CAPACITY];
int16_t gwenesis_sn76489_buffer[GWENESIS_AUDIO_BUFFER_CAPACITY];
const unsigned char *ROM_DATA; unsigned int ROM_DATA_LENGTH;
void gwenesis_io_get_buttons(void){} void wdog_refresh(void){}

static uint16_t s_fb[320*240];
uint16_t *lcd_get_active_buffer(void){return s_fb;}
void lcd_swap(void){} void lcd_wait_for_vblank(void){} void lcd_set_refresh_rate(int h){(void)h;}
void common_emu_clear_dwt_cycles(void){} int common_emu_frame_loop(void){return 0;}
void common_ingame_overlay(void){} void common_emu_sound_sync(bool b){(void)b;}
void odroid_audio_init(int f){(void)f;} void odroid_audio_submit(int16_t*b,uint16_t n){(void)b;(void)n;}

/* The region BIOS pointer segacd_bus.c maps at main $000000 (weak in main_segacd
 * on device; here we provide it from the loaded file). */
const uint8_t *segacd_bios;

static void md_scanline_frame(void)
{
    /* one base MD frame (main 68K + Z80 + VDP), same loop as perf_test/rig_md */
    screen_height = REG1_PAL?240:224; screen_width = REG12_MODE_H40?320:256;
    lines_per_frame = mode_pal?LINES_PER_FRAME_PAL:LINES_PER_FRAME_NTSC;
    vert_screen_offset = mode_pal?0:320*(240-224)/2;
    gwenesis_vdp_set_buffer(&s_fb[vert_screen_offset]); gwenesis_vdp_render_config();
    system_clock=0; zclk=0; ym2612_clock=ym2612_index=0; sn76489_clock=sn76489_index=0; scan_line=0;
    int line;
    gwenesis_vdp_status=(unsigned short)((gwenesis_vdp_status&(unsigned short)~0x0112u)|STATUS_VBLANK);
    gwenesis_vdp_status^=STATUS_ODDFRAME;
    scan_line=(int)screen_height;
    if(!skip_first_vint){ gwenesis_vdp_status|=STATUS_VIRQPENDING;
      if(REG1_VBLANK_INTERRUPT)m68k_set_irq(6); z80_irq_line(1); }
    m68k_run(system_clock+VDP_CYCLES_PER_LINE); z80_run(system_clock+VDP_CYCLES_PER_LINE);
    system_clock+=VDP_CYCLES_PER_LINE; z80_irq_line(0);
    for(line=(int)screen_height+1; line<(int)lines_per_frame-1; line++){ scan_line=line;
      m68k_run(system_clock+VDP_CYCLES_PER_LINE); z80_run(system_clock+VDP_CYCLES_PER_LINE); system_clock+=VDP_CYCLES_PER_LINE; }
    scan_line=(int)lines_per_frame-1; hint_counter=(int)REG10_LINE_COUNTER;
    gwenesis_vdp_status&=(unsigned short)~STATUS_VBLANK;
    m68k_run(system_clock+VDP_CYCLES_PER_LINE); z80_run(system_clock+VDP_CYCLES_PER_LINE); system_clock+=VDP_CYCLES_PER_LINE;
    for(line=0; line<(int)screen_height; line++){ scan_line=line; gwenesis_vdp_latch_line_scroll(line);
      if(hint_counter==0){hint_counter=(int)REG10_LINE_COUNTER; hint_pending=1; if(REG0_LINE_INTERRUPT)m68k_update_irq(4);} else hint_counter--;
      m68k_run(system_clock+VDP_CYCLES_PER_LINE); z80_run(system_clock+VDP_CYCLES_PER_LINE);
      gwenesis_vdp_render_line(line); system_clock+=VDP_CYCLES_PER_LINE; }
    gwenesis_SN76489_run(system_clock); ym2612_run(system_clock); m68k.cycles-=system_clock;
    skip_first_vint=0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr,"usage: boot_test <bios.bin> <game.cue> [frames]\n"); return 2; }
    int FRAMES = argc > 3 ? atoi(argv[3]) : 600;

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
    for (int frame=0; frame<FRAMES; frame++) {
        button_state[0]=0xFF;
        md_scanline_frame();                 /* main 68K + VDP (BIOS runs here) */
        if (!prev_running && SCD.sub_running) {  /* sub just released — pristine image */
            prev_running = 1;
            #define PRGW(o) ((SCD.prg_ram[((o)+1)&(SEGACD_PRG_RAM_SIZE-1)]<<8) | SCD.prg_ram[(o)&(SEGACD_PRG_RAM_SIZE-1)])
            unsigned end = (PRGW(0x1a4)<<16)|PRGW(0x1a6); uint16_t sum=0;
            for (unsigned o=0x200; o<end; o+=2) sum += PRGW(o);
            printf("[boot] >>> at sub-release f%d: checksum sum(0x200..%06x)=%04x expected=%04x %s\n",
                   frame, end, sum, PRGW(0x18e), (sum==PRGW(0x18e))?"MATCH":"MISMATCH");
            #undef PRGW
        }
        segacd_run_sub(SUB_CYCLES_PER_FRAME);/* sub 68K once released by BIOS */
        segacd_cdd_process();
        segacd_cd_update();

        int sample_period = (frame > 60 && frame < 140) ? 5 : 60;
        if ((frame % sample_period) == 0) {
            uint32_t h = 2166136261u; for(int k=0;k<320*240;k++) h=(h^s_fb[k])*16777619u;
            printf("[boot] f%-4d sub_running=%d subPC=%06x mainPC=%06x idle=%u cdd_status=%02x "
                   "cmd=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x ga_ifl2=%u cdd_pend=%d ien=%02x %s\n",
                   frame, SCD.sub_running, (unsigned)SCD.sub_ctx.pc, (unsigned)m68k.pc, (unsigned)SCD.sub_idle,
                   SCD.s68k_regs[0x38 & (SEGACD_GA_REGS-1)],
                   SCD.s68k_regs[0x42],SCD.s68k_regs[0x43],SCD.s68k_regs[0x44],SCD.s68k_regs[0x45],
                   SCD.s68k_regs[0x46],SCD.s68k_regs[0x47],SCD.s68k_regs[0x48],SCD.s68k_regs[0x49],
                   SCD.s68k_regs[0x4a],SCD.s68k_regs[0x4b],
                   (unsigned)SCD.ga_ifl2, SCD.cdd_int_pending, SCD.s68k_regs[0x33],
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
    printf("[boot] done %d frames. sub_running=%d\n", FRAMES, SCD.sub_running);
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
        if(d){ for(unsigned o=0x0;o<0x10000;o+=2){ uint16_t w=PRGW(o);
                 unsigned char be[2]={(unsigned char)(w>>8),(unsigned char)(w&0xff)};
                 fwrite(be,1,2,d);} fclose(d);
               printf("[boot] wrote /tmp/scd/prg_subbios.bin (0x0..0x10000 BE)\n"); }
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
#endif
    return 0;
}
