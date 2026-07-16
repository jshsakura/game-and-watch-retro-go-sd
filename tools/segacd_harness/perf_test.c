/* Fast host (x86) measurement of the Sega CD dual-68K cost RATIO.
 *
 * The QEMU M7 rig is honest about absolute instruction counts but is ~1 min per
 * frame for the 68K core — impractical for gameplay-length runs. What decides
 * "does the dual-68K fit" is the RATIO sub_cost/emu (how much a second 68K's
 * cycle budget adds to an MD frame), and that ratio is a property of the same
 * interpreter running the same ops — it transfers x86->ARM. Here we get it at
 * full x86 speed (thousands of frames/s).
 *
 *   ./perf_test <md_rom.bin> [frames]
 *
 * Frame loop copied from linux/gwenesis/main.c (same as rig_md). Per frame:
 * emu = full MD frame; sub = marginal cost of SUB_CYCLES_PER_FRAME more 68K
 * cycles (snapshot/restore, main untouched). Prints avg dual/emu multiplier.
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

#define VINT_H32_CYCLES 770u
#define VINT_H40_CYCLES 788u
#define SUB_CYCLES_PER_FRAME 208333u   /* 12.5 MHz / 60 fps */

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

static double now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e9 + t.tv_nsec; }

/* Time just the main-68K interpretation within the normal frame (cheap on x86).
 * Sub-68K runs 12.5MHz vs main 7.67MHz over ~the same op mix, so its cost is
 * cpu68k x (208333 / main_cycles_per_frame). Safe (no run into unmapped space). */
static double g_cpu68k;
static unsigned g_main_cycles;   /* main-68K cycles advanced this frame */
static uint32_t g_audio_hash = 2166136261u;   /* YM2612+PSG output hash (losslessness) */
static double g_ym_ns;                          /* time in ym2612_run */
static double g_psg_ns;                          /* time in gwenesis_SN76489_run */
#define RUN68K(t) do { unsigned _b=m68k.cycles; double _c=now_ns(); m68k_run(t); \
    g_cpu68k += now_ns()-_c; g_main_cycles += (unsigned)(m68k.cycles-_b); } while(0)

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr,"usage: perf_test <rom.bin> [frames]\n"); return 2; }
    int FRAMES = argc > 2 ? atoi(argv[2]) : 600;

    FILE *f = fopen(argv[1],"rb");
    if(!f){perror("rom");return 1;}
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    unsigned char *rom = malloc(n); if(fread(rom,1,n,f)!=(size_t)n){return 1;} fclose(f);

    /* HARNESS-FIRST: the 68K reads code through the bus ROM_SWAP path, which is
     * NOT the same path load_cartridge reads the header through — so a correct
     * header does NOT prove correct code. update_gwenesis_rom.sh pair-swaps the
     * .bin before the core sees it; do the same, or the 68K executes wrong-endian
     * garbage (illegal-op storm, no register writes, all games hash identically —
     * the tell that the games are NOT running). */
    for(long i=0;i+1<n;i+=2){ unsigned char t=rom[i]; rom[i]=rom[i+1]; rom[i+1]=t; }
    ROM_DATA = rom; ROM_DATA_LENGTH = (unsigned)n;
    /* Order matches linux/gwenesis/main.c (works on x86): power_on builds the
     * memory_map + m68k_init, THEN reset_emulation pulse-resets. */
    load_cartridge(); power_on(); reset_emulation();
    skip_first_vint=1; hint_counter=0xff;
    gwenesis_vdp_set_buffer(&s_fb[0]);

    double tot_emu=0, tot_sub=0;
    for(int frame=0; frame<FRAMES; frame++){
        button_state[0]=0xFF;
        screen_height = REG1_PAL?240:224; screen_width = REG12_MODE_H40?320:256;
        lines_per_frame = mode_pal?LINES_PER_FRAME_PAL:LINES_PER_FRAME_NTSC;
        vert_screen_offset = mode_pal?0:320*(240-224)/2;
        gwenesis_vdp_set_buffer(&s_fb[vert_screen_offset]); gwenesis_vdp_render_config();
        system_clock=0; zclk=0; ym2612_clock=ym2612_index=0; sn76489_clock=sn76489_index=0; scan_line=0;

        double t0=now_ns();
        { const unsigned vc = REG12_MODE_H40?VINT_H40_CYCLES:VINT_H32_CYCLES; int line; (void)vc;
          gwenesis_vdp_status=(unsigned short)((gwenesis_vdp_status&(unsigned short)~0x0112u)|STATUS_VBLANK);
          gwenesis_vdp_status^=STATUS_ODDFRAME;
          scan_line=(int)screen_height;
          if(!skip_first_vint){ gwenesis_vdp_status|=STATUS_VIRQPENDING;
            if(REG1_VBLANK_INTERRUPT)m68k_set_irq(6); z80_irq_line(1); }
          RUN68K(system_clock+VDP_CYCLES_PER_LINE); z80_run(system_clock+VDP_CYCLES_PER_LINE);
          system_clock+=VDP_CYCLES_PER_LINE; z80_irq_line(0);
          for(line=(int)screen_height+1; line<(int)lines_per_frame-1; line++){ scan_line=line;
            RUN68K(system_clock+VDP_CYCLES_PER_LINE); z80_run(system_clock+VDP_CYCLES_PER_LINE); system_clock+=VDP_CYCLES_PER_LINE; }
          scan_line=(int)lines_per_frame-1; hint_counter=(int)REG10_LINE_COUNTER;
          gwenesis_vdp_status&=(unsigned short)~STATUS_VBLANK;
          RUN68K(system_clock+VDP_CYCLES_PER_LINE); z80_run(system_clock+VDP_CYCLES_PER_LINE); system_clock+=VDP_CYCLES_PER_LINE;
          for(line=0; line<(int)screen_height; line++){ scan_line=line; gwenesis_vdp_latch_line_scroll(line);
            if(hint_counter==0){hint_counter=(int)REG10_LINE_COUNTER; hint_pending=1; if(REG0_LINE_INTERRUPT)m68k_update_irq(4);} else hint_counter--;
            RUN68K(system_clock+VDP_CYCLES_PER_LINE); z80_run(system_clock+VDP_CYCLES_PER_LINE);
            gwenesis_vdp_render_line(line); system_clock+=VDP_CYCLES_PER_LINE; }
          skip_first_vint=0; }
        double p0=now_ns(); gwenesis_SN76489_run(system_clock); g_psg_ns += now_ns()-p0;
        double y0=now_ns(); ym2612_run(system_clock); g_ym_ns += now_ns()-y0;
        m68k.cycles-=system_clock;
        double t1=now_ns();
        /* Losslessness check: hash BOTH sound chip outputs each frame. A correct
         * sound optimization must leave this byte-identical. */
        for(int k=0;k<GWENESIS_AUDIO_BUFFER_CAPACITY;k++){
            g_audio_hash=(g_audio_hash^(uint32_t)(uint16_t)gwenesis_ym2612_buffer[k])*16777619u;
            g_audio_hash=(g_audio_hash^(uint32_t)(uint16_t)gwenesis_sn76489_buffer[k])*16777619u; }

        /* sub cost = cpu68k scaled to the sub's 12.5MHz cycle budget. */
        double sub = (g_main_cycles>0) ? g_cpu68k * (double)SUB_CYCLES_PER_FRAME / g_main_cycles : 0;
        if(frame>=30){ tot_emu += (t1-t0); tot_sub += sub; }  /* skip boot */
        g_cpu68k=0; g_main_cycles=0;
    }
    double mul = (tot_emu>0)?(tot_emu+tot_sub)/tot_emu:0;
    printf("emu=%.0fns sub=%.0fns  ->  dual/MD multiplier = x%.2f  (+PCM/ASIC ~+15%% => ~x%.2f)\n",
           tot_emu, tot_sub, mul, mul*1.15);
    printf("YM2612: %.0fns  SN76489: %.0fns  audio_hash=%08x\n", g_ym_ns, g_psg_ns, g_audio_hash);
    return 0;
}
