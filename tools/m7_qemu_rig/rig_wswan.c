/* WonderSwan (oswan) on the M7 QEMU rig: the exact DEVICE core sources
 * (WS.c, ws_fileio.c, WSRender.c, WSApu.c, nec.c) with the device's own
 * defines (GNW_WSWAN, NOSDL_FB, SOUND_ON, SOUND_EMULATION), running as a REAL
 * ARMv7-M instruction stream. A CMSDK-timer delta under -icount shift=0 is an
 * executed-instruction count, so this prints instructions/frame split three
 * ways — CPU emulation, PPU (per-scanline render), and the front-end blit.
 *
 * The device gates the per-scanline render with ws_render_enabled (set per
 * frame in main_wswan.c). We build the rig twice: RIG_RENDER=1 measures
 * WsRun = emu+ppu, RIG_RENDER=0 measures WsRun = emu only (render skipped,
 * same CPU state either way). The PPU cost is the difference, and run_wswan.sh
 * subtracts them into one ledger line.
 *
 * The ROM is linked in (objcopy -I binary): _binary_rom_ws_start/end.
 * FrameBuffer hashes are printed so a run can be cross-checked against an x86
 * host build of the same core (same ROM + input script => same hashes).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef RIG_FRAMES
#define RIG_FRAMES 3000
#endif
#ifndef RIG_RENDER
#define RIG_RENDER 1        /* 1 = render on (emu+ppu), 0 = render off (emu only) */
#endif
#define RIG_WINDOW 200      /* print a ledger line every N frames */

/* ---- oswan core entry points (forward-declared, as main_wswan.c does, to
 *      dodge WS.h's generic globals clashing with anything). ---- */
void     WsInit(void);
void     WsReset(void);
uint32_t WsRun(void);
int      ws_create_from_flash(const uint8_t *data, uint32_t size);

extern uint16_t FrameBuffer[240 * 144];   /* WSRender.c render target */
extern int      ws_render_enabled;         /* WSRender.c per-scanline gate */
extern uint8_t  Layer[3];                  /* BG/FG/sprite enable (profiling lever) */

/* RIG_LAYERS bitmask: bit0=BG bit1=FG bit2=sprite. Default 7 = all on. Lets a
 * profiling build disable a layer to attribute the PPU cost. */
#ifndef RIG_LAYERS
#define RIG_LAYERS 7
#endif

/* ---- device-glue stubs, identical to main_wswan.c's no-ops ---- */
char gameName[512] = "/rig/game";          /* WsLoadEeprom does strrchr(gameName,'/')+1 */
void graphics_paint(void)  { }
void Sound_APU_Start(void) { }
void Sound_APU_End(void)   { }
void Sound_APUClose(void)  { }
void Pause_Sound(void)     { }

/* rig_runtime.c */
void     rig_timer_init(void);
uint32_t rig_timer_now(void);
uint32_t rig_calibrate(uint32_t n);

extern unsigned char _binary_rom_ws_start[];
extern unsigned char _binary_rom_ws_end[];

/* ---- deterministic input script (device WsInputGetState bit layout: Y1-4 =
 *      bits 0-3, X-pad = bits 4-7, OPTION=8, START=9, A=10, B=11). Generic
 *      "tap START then A" to nudge a title screen into gameplay; keep it
 *      identical to any host cross-check harness. ---- */
static int s_frame;
uint32_t WsInputGetState(void)
{
    uint32_t k = 0;
    if (s_frame >= 60  && s_frame < 76)  k |= 0x0200;            /* START */
    if (s_frame >= 200 && (s_frame % 90) < 8) k |= 0x0400;      /* A tap */
    if (s_frame >= 400 && (s_frame % 300) < 8) k |= 0x0020;     /* X2 right nudge */
    return k;
}

/* ---- blit: the FIT path from main_wswan.c's screen_blit_nn (default WS
 *      scaling), into a fake 320x240 LCD buffer. ---- */
#define GW_LCD_WIDTH  320
#define GW_LCD_HEIGHT 240
#define WS_WIDTH   224
#define WS_HEIGHT  144
#define WS_STRIDE  240
#define WS_XOFF    8
static uint16_t s_lcd[GW_LCD_WIDTH * GW_LCD_HEIGHT];

static void blit_fit(void)
{
    const int dest_width  = 320;
    const int dest_height = (WS_HEIGHT * 320 + WS_WIDTH / 2) / WS_WIDTH;  /* ~206 */
    int w1 = WS_WIDTH, h1 = WS_HEIGHT, w2 = dest_width, h2 = dest_height;
    int x_ratio = (int)((w1 << 16) / w2) + 1;
    int y_ratio = (int)((h1 << 16) / h2) + 1;
    int hpad = (320 - dest_width) / 2;
    int wpad = (240 - dest_height) / 2;

    memset(s_lcd, 0, sizeof(s_lcd));   /* lcd_clear_active_buffer() */
    for (int i = 0; i < h2; i++) {
        for (int j = 0; j < w2; j++) {
            int x2 = ((j * x_ratio) >> 16);
            int y2 = ((i * y_ratio) >> 16);
            s_lcd[((i + wpad) * GW_LCD_WIDTH) + j + hpad] =
                FrameBuffer[(y2 * WS_STRIDE) + x2 + WS_XOFF];
        }
    }
}

static uint32_t fnv1a(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    while (len--) { h ^= *p++; h *= 16777619u; }
    return h;
}

int main(void)
{
    unsigned char *rom = _binary_rom_ws_start;
    uint32_t rom_len = (uint32_t)(_binary_rom_ws_end - _binary_rom_ws_start);

    rig_timer_init();
    uint32_t cal_ticks = rig_calibrate(1000000);
    uint32_t ipt_x1000 = (uint32_t)((3000000ull * 1000ull) / (cal_ticks ? cal_ticks : 1));
    printf("[ws-qemu] cal: 3.0M insns = %lu ticks -> %lu.%03lu insn/tick\n",
           (unsigned long)cal_ticks,
           (unsigned long)(ipt_x1000 / 1000), (unsigned long)(ipt_x1000 % 1000));
    printf("[ws-qemu] rom len=%lu frames=%d render=%d\n",
           (unsigned long)rom_len, RIG_FRAMES, RIG_RENDER);

    Layer[0] = (RIG_LAYERS >> 0) & 1;
    Layer[1] = (RIG_LAYERS >> 1) & 1;
    Layer[2] = (RIG_LAYERS >> 2) & 1;

    WsInit();
    if (!ws_create_from_flash(rom, rom_len)) {
        printf("[ws-qemu] ws_create_from_flash FAILED\n");
        return 1;
    }
    WsReset();

    uint32_t run_hash = 2166136261u;
    uint64_t win_run = 0, win_blit = 0, tot_run = 0, tot_blit = 0;

    for (s_frame = 0; s_frame < RIG_FRAMES; s_frame++) {
        ws_render_enabled = RIG_RENDER;

        uint32_t t0 = rig_timer_now();
        WsRun();
        uint32_t t1 = rig_timer_now();
        blit_fit();
        uint32_t t2 = rig_timer_now();

        win_run  += (uint32_t)(t1 - t0);
        win_blit += (uint32_t)(t2 - t1);

        uint32_t h = fnv1a(FrameBuffer, sizeof(FrameBuffer));
        run_hash = (run_hash ^ h) * 16777619u;

        if ((s_frame + 1) % RIG_WINDOW == 0) {
            uint64_t run_i  = win_run  * ipt_x1000 / 1000 / RIG_WINDOW;
            uint64_t blit_i = win_blit * ipt_x1000 / 1000 / RIG_WINDOW;
            printf("w%05d run=%lu blit=%lu insn/frame fb=%08x\n",
                   s_frame + 1, (unsigned long)run_i, (unsigned long)blit_i, (unsigned)h);
            tot_run += win_run;
            tot_blit += win_blit;
            win_run = win_blit = 0;
        }
    }

    uint64_t frames = (RIG_FRAMES / RIG_WINDOW) * RIG_WINDOW;
    if (frames == 0) frames = 1;
    printf("[ws-qemu] done %d frames render=%d RUNHASH=%08x avg run=%lu blit=%lu insn/frame\n",
           RIG_FRAMES, RIG_RENDER, (unsigned)run_hash,
           (unsigned long)(tot_run * ipt_x1000 / 1000 / frames),
           (unsigned long)(tot_blit * ipt_x1000 / 1000 / frames));
    return 0;
}
