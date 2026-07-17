/* Mega Drive (gwenesis) on the M7 QEMU rig: executed-instruction baseline.
 *
 * This is the DENOMINATOR for the Sega/Mega CD feasibility question. It runs
 * the single-68K gwenesis core — the same core sources the device links — as a
 * real ARMv7-M instruction stream (QEMU mps2-an500, -icount shift=0), so a
 * CMSDK-timer delta is an executed-instruction count. It prints per-window
 * emu=/blit= instructions/frame, exactly like rig_vb.c / rig_wswan.c.
 *
 * Why it exists: Sega CD adds a SECOND 68000 (12.5 MHz) + Word-RAM graphics
 * ASIC + RF5C164 PCM on top of THIS core. To know whether that is real-time on
 * the device we must first know what one 68K MD frame costs here, then measure
 * the multiplier a second 68K context adds (rig_mcd.c, next). Absolute fps
 * still belongs to the device — QEMU has no caches/wait-states.
 *
 * The frame loop below is copied VERBATIM (scan order, VINT delay, H-INT rule)
 * from linux/gwenesis/main.c:run_gwenesis_emulation(), minus SDL/save/audio —
 * so its frame hashes are comparable with the host harness for the same ROM.
 *
 * ROM is linked in as a binary blob: _binary_rom_md_start/end (run_md.sh).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gwenesis_bus.h"
#include "gwenesis_io.h"
#include "gwenesis_vdp.h"
#include "m68k.h"
#include "z80inst.h"
#include "ym2612.h"

#ifndef RIG_FRAMES
#define RIG_FRAMES 3000
#endif
#define RIG_WINDOW 20

/* VINT delay cycles — defined in linux/gwenesis/main.c, not a core header. */
#define VINT_H32_CYCLES 770u
#define VINT_H40_CYCLES 788u

/* Core-owned globals (defined in gwenesis vdp/io); we only read/latch them. */
extern unsigned char gwenesis_vdp_regs[0x20];
extern unsigned short gwenesis_vdp_status;
extern int hint_pending;
extern unsigned int screen_width, screen_height;
extern int mode_pal;

/* rig_runtime.c */
void rig_timer_init(void);
uint32_t rig_timer_now(void);
uint32_t rig_calibrate(uint32_t n);

/* Time just the main-68K interpretation, separately from Z80/VDP/sound. The
 * Sega CD adds a SECOND 68K at 12.5 MHz vs this one's 7.67 MHz, so its cost is
 * ~this x (12.5/7.67) = x1.63 (Musashi interpreter cost is ~linear in cycles).
 * That lets a single MD run estimate the dual-68K frame without a working CD
 * core: dual_est = emu_full + 1.63 x cpu68k. */
static uint64_t g_cpu68k_ticks;
#define RUN68K(t) do { uint32_t _c = rig_timer_now(); m68k_run(t); \
                       g_cpu68k_ticks += (uint32_t)(rig_timer_now() - _c); } while (0)
#define SUB68K_SCALE_X1000  1630u   /* 12.5 MHz / 7.67 MHz */

extern unsigned char _binary_rom_md_start[];
extern unsigned char _binary_rom_md_end[];

/* ---- device allocators: on the rig, plain heap (PSRAM arena via _sbrk) ---- */
void ahb_init(void) {}
void itc_init(void) {}
void *ahb_malloc(size_t s) { return malloc(s); }
void *ahb_calloc(size_t n, size_t s) { return calloc(n, s); }
void *itc_malloc(size_t s) { return malloc(s); }
void *itc_calloc(size_t n, size_t s) { return calloc(n, s); }
void *ram_malloc(size_t s) { return malloc(s); }
void *ram_calloc(size_t n, size_t s) { return calloc(n, s); }

/* ---- harness-owned globals (core defines its own; these are the shell's) ---- */
int system_clock;
int scan_line;
unsigned int lines_per_frame = LINES_PER_FRAME_NTSC;
int hint_counter;
int skip_first_vint;
int drawFrame = 1;
int sn76489_clock;
int sn76489_index;
int ym2612_clock;
int ym2612_index;
int vert_screen_offset;
int hori_screen_offset;

int16_t gwenesis_ym2612_buffer[GWENESIS_AUDIO_BUFFER_CAPACITY];
int16_t gwenesis_sn76489_buffer[GWENESIS_AUDIO_BUFFER_CAPACITY];

/* ROM pointer/length the bus reads (harness-owned in linux/gwenesis/main.c).
 * Under TARGET_GNW m68k.h declares ROM_DATA as `const unsigned char *`. */
const unsigned char *ROM_DATA;
unsigned int ROM_DATA_LENGTH;

/* Core callbacks/stubs. button_state is filled by input_script() each frame,
 * so the per-read refresh hook can be empty; wdog is a no-op on the rig. */
void gwenesis_io_get_buttons(void) {}
void wdog_refresh(void) {}

/* ---- one LCD framebuffer, the device's LCD geometry ---- */
#define GW_LCD_WIDTH  320
#define GW_LCD_HEIGHT 240
static uint16_t s_fb[GW_LCD_WIDTH * GW_LCD_HEIGHT];

/* ---- shell stubs the frame loop touches ---- */
uint16_t *lcd_get_active_buffer(void) { return s_fb; }
void lcd_swap(void) {}
void lcd_wait_for_vblank(void) {}
void lcd_set_refresh_rate(int hz) { (void)hz; }
void common_emu_clear_dwt_cycles(void) {}
int  common_emu_frame_loop(void) { return 0; }         /* always draw */
void common_ingame_overlay(void) {}
void common_emu_sound_sync(bool b) { (void)b; }
void odroid_audio_init(int f) { (void)f; }
void odroid_audio_submit(int16_t *b, uint16_t n) { (void)b; (void)n; }

static uint32_t fnv1a(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t h = 2166136261u;
    while (len--) { h ^= *p++; h *= 16777619u; }
    return h;
}

/* Deterministic input: press START briefly (past intros), no held keys. */
static void input_script(int frame)
{
    unsigned char k = 0;
    if (frame >= 250 && (frame % 250) < 16) k |= (1u << PAD_S);
    button_state[0] = (unsigned char)~k;   /* core wants active-low */
}

int main(void)
{
    unsigned char *rom = _binary_rom_md_start;
    uint32_t rom_len = (uint32_t)(_binary_rom_md_end - _binary_rom_md_start);

    rig_timer_init();
    uint32_t cal_ticks = rig_calibrate(1000000);
    uint32_t ipt_x1000 = (uint32_t)((3000000ull * 1000ull) / (cal_ticks ? cal_ticks : 1));
    printf("[md-qemu] cal: 3.0M insns = %lu ticks -> %lu.%03lu insn/tick\n",
           (unsigned long)cal_ticks,
           (unsigned long)(ipt_x1000 / 1000), (unsigned long)(ipt_x1000 % 1000));

    /* ROM into the core (bus reads ROM_DATA; load_cartridge pair-swaps it). */
    ROM_DATA = rom;
    ROM_DATA_LENGTH = rom_len;
    printf("[md-qemu] rom len=%lu frames=%d\n", (unsigned long)rom_len, RIG_FRAMES);

    load_cartridge();
    m68k_init();
    reset_emulation();
    power_on();
    skip_first_vint = 1;
    hint_counter = 0xff;

    gwenesis_vdp_set_buffer(&s_fb[0]);

    uint32_t run_hash = 2166136261u;
    uint64_t win_emu = 0, win_blit = 0, tot_emu = 0, tot_blit = 0;

    for (int frame = 0; frame < RIG_FRAMES; frame++) {
        input_script(frame);

        screen_height = REG1_PAL ? 240 : 224;
        screen_width  = REG12_MODE_H40 ? 320 : 256;
        lines_per_frame = mode_pal ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
        vert_screen_offset = mode_pal ? 0 : 320 * (240 - 224) / 2;
        gwenesis_vdp_set_buffer(&s_fb[vert_screen_offset]);
        gwenesis_vdp_render_config();

        system_clock = 0; zclk = 0;
        ym2612_clock = ym2612_index = 0;
        sn76489_clock = sn76489_index = 0;
        scan_line = 0;

        uint32_t t0 = rig_timer_now();
        {
            const unsigned vint_cycles = REG12_MODE_H40 ? VINT_H40_CYCLES : VINT_H32_CYCLES;
            int line;
            gwenesis_vdp_status = (unsigned short)((gwenesis_vdp_status & (unsigned short)~0x0112u) | STATUS_VBLANK);
            gwenesis_vdp_status ^= STATUS_ODDFRAME;

            scan_line = (int)screen_height;
            if (!skip_first_vint) {
                gwenesis_vdp_status |= STATUS_VIRQPENDING;
                if (REG1_VBLANK_INTERRUPT != 0) m68k_set_irq(6);
                z80_irq_line(1);
            }
            m68k_run(system_clock + VDP_CYCLES_PER_LINE);
            z80_run(system_clock + VDP_CYCLES_PER_LINE);
            system_clock += VDP_CYCLES_PER_LINE;
            z80_irq_line(0);

            for (line = (int)screen_height + 1; line < (int)lines_per_frame - 1; line++) {
                scan_line = line;
                m68k_run(system_clock + VDP_CYCLES_PER_LINE);
                z80_run(system_clock + VDP_CYCLES_PER_LINE);
                system_clock += VDP_CYCLES_PER_LINE;
            }

            scan_line = (int)lines_per_frame - 1;
            hint_counter = (int)REG10_LINE_COUNTER;
            gwenesis_vdp_status &= (unsigned short)~STATUS_VBLANK;
            m68k_run(system_clock + VDP_CYCLES_PER_LINE);
            z80_run(system_clock + VDP_CYCLES_PER_LINE);
            system_clock += VDP_CYCLES_PER_LINE;

            for (line = 0; line < (int)screen_height; line++) {
                scan_line = line;
                gwenesis_vdp_latch_line_scroll(line);
                if (hint_counter == 0) {
                    hint_counter = (int)REG10_LINE_COUNTER;
                    hint_pending = 1;
                    if (REG0_LINE_INTERRUPT) m68k_update_irq(4);
                } else hint_counter--;
                m68k_run(system_clock + VDP_CYCLES_PER_LINE);
                z80_run(system_clock + VDP_CYCLES_PER_LINE);
                gwenesis_vdp_render_line(line);
                system_clock += VDP_CYCLES_PER_LINE;
            }
            skip_first_vint = 0;
        }
        gwenesis_SN76489_run(system_clock);
        ym2612_run(system_clock);
        m68k.cycles -= system_clock;
        uint32_t t1 = rig_timer_now();

        win_emu += (uint32_t)(t1 - t0);

        uint32_t h = fnv1a(s_fb, sizeof(s_fb));
        run_hash = (run_hash ^ h) * 16777619u;

        if ((frame + 1) % RIG_WINDOW == 0) {
            uint64_t emu_i = win_emu * ipt_x1000 / 1000 / RIG_WINDOW;
            printf("w%05d emu=%lu insn/frame fb=%08x\n",
                   frame + 1, (unsigned long)emu_i, (unsigned)h);
            tot_emu += win_emu; tot_blit += win_blit;
            win_emu = win_blit = 0;
        }
    }

    uint64_t frames = (RIG_FRAMES / RIG_WINDOW) * RIG_WINDOW;
    if (!frames) frames = 1;
    printf("[md-qemu] done %d frames RUNHASH=%08x avg emu=%lu insn/frame\n",
           RIG_FRAMES, (unsigned)run_hash,
           (unsigned long)(tot_emu * ipt_x1000 / 1000 / frames));
    return 0;
}
