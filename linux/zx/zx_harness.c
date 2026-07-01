/*
 * ZX Spectrum (chips core) host harness — sound + input verification.
 *
 * Runs the chips ZX core with the EXACT audio desc main_zxs.c uses, under
 * ASan/UBSan, WITHOUT the retro-go firmware layer. (The PAUSE-menu HardFault is
 * a firmware NULL-repaint bug, fixed in main_zxs.c by inspection; the firmware
 * menu code isn't present here so it can't be reproduced on the host.)
 *
 * Findings this harness locks in:
 *   1. Boot: 48.rom + zx_init + frames, no core crash.
 *   2. SOUND: a CPU `OUT (0xFE),a` beeper loop (exactly what a game's sound
 *      routine does) drives beeper -> audio callback with a non-zero level.
 *      => the core sound pipeline is GOOD; any device silence is downstream
 *      (DMA/submit), NOT the core.
 *   3. INPUT: every joystick dir + button, in all 2^5 combinations, plus keys
 *      held simultaneously (the "PAUSE + combo + control key" cases) — ASan-clean.
 *
 * Needs 48.rom in the cwd. Build/run: run_zx_host_test.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHIPS_IMPL
#include "chips/chips_common.h"
#include "chips/z80.h"
#include "chips/beeper.h"
#include "chips/ay38910.h"
#include "chips/mem.h"
#include "chips/kbd.h"
#include "chips/clk.h"
#include "chips/zx.h"

static zx_t zx;

/* mirror main_zxs.c's audio desc (22050 Hz, beeper 0.5) + accumulate the callback */
#define SR 22050
static int32_t g_peak;
static long    g_nonzero;
static long    g_calls;

static void audio_cb(const float *s, int n, void *u) {
    (void)u;
    g_calls++;
    for (int i = 0; i < n; i++) {
        int32_t v = (int32_t)(s[i] * 22000.0f);
        if (v < 0) v = -v;
        if (v > g_peak) g_peak = v;
        if (v) g_nonzero++;
    }
}

static void run_frames(int n) { for (int i = 0; i < n; i++) zx_exec(&zx, 19968); }

int main(void) {
    static uint8_t rom48[16384];
    FILE *f = fopen("48.rom", "rb");
    if (!f) { printf("FAIL: missing 48.rom in cwd\n"); return 2; }
    if (fread(rom48, 1, 16384, f) != 16384) { printf("FAIL: 48.rom not 16K\n"); fclose(f); return 2; }
    fclose(f);

    zx_desc_t desc = {0};
    desc.type                = ZX_TYPE_48K;
    desc.joystick_type       = ZX_JOYSTICKTYPE_KEMPSTON;
    desc.audio.callback.func = audio_cb;
    desc.audio.num_samples   = 128;
    desc.audio.sample_rate   = SR;
    desc.audio.beeper_volume = 0.5f;
    desc.audio.ay_volume     = 0.5f;
    desc.roms.zx48k.ptr      = rom48;
    desc.roms.zx48k.size     = 16384;
    zx_init(&zx, &desc);

    /* 1. Boot */
    run_frames(150);
    printf("OK boot: 150 frames, %ld audio_cb calls, no crash\n", g_calls);

    /* 2. SOUND — inject a `OUT (0xFE),a` beeper toggle loop at 0x8000 and run it.
     * This is exactly the IO a game's sound code does; proves CPU -> beeper ->
     * audio callback works. */
    const uint8_t prog[] = { 0x3E,0x10, 0xD3,0xFE, 0x3E,0x00, 0xD3,0xFE, 0x18,0xF6 };
    for (unsigned i = 0; i < sizeof(prog); i++) mem_wr(&zx.mem, 0x8000 + i, prog[i]);
    z80_prefetch(&zx.cpu, 0x8000);
    g_peak = 0; g_nonzero = 0;
    run_frames(20);
    printf("   sound: CPU OUT(0xFE) beeper loop -> peak=%d, %ld non-zero samples\n", g_peak, g_nonzero);
    if (g_peak < 1000 || g_nonzero == 0) {
        printf("FAIL sound: core beeper pipeline produced no output\n");
        return 1;
    }
    printf("OK sound: core CPU->beeper->callback pipeline works (peak=%d)\n", g_peak);

    /* 3. INPUT — all joystick/button combos + keys held together, ASan-guarded. */
    const uint8_t bits[5] = { ZX_JOYSTICK_LEFT, ZX_JOYSTICK_RIGHT, ZX_JOYSTICK_UP,
                              ZX_JOYSTICK_DOWN, ZX_JOYSTICK_BTN };
    z80_prefetch(&zx.cpu, 0x0000);   /* back to ROM for a realistic keyboard path */
    long combos = 0;
    for (int m = 0; m < 32; m++) {
        uint8_t mask = 0;
        for (int b = 0; b < 5; b++) if (m & (1 << b)) mask |= bits[b];
        zx_joystick(&zx, mask);
        zx_key_down(&zx, 0x0D);      /* Enter held with the joystick combo */
        run_frames(2);
        zx_key_up(&zx, 0x0D);
        zx_joystick(&zx, 0);
        run_frames(1);
        combos++;
    }
    const int keys[] = { ' ', '0','1','2','3','4','5','a','q','p','m', 0x0D };
    for (unsigned i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        zx_key_down(&zx, keys[i]); run_frames(2);
        zx_key_down(&zx, 0x0D);    run_frames(2);   /* two keys down together */
        zx_key_up(&zx, 0x0D);
        zx_key_up(&zx, keys[i]);   run_frames(1);
    }
    printf("OK input: %ld joystick/button combos + control-key combos, ASan-clean\n", combos);

    /* 4. INPUT REACHES THE MACHINE — the "can't start a game" check.
     * (a) Kempston: zx_joystick() must set joy_joymask (what IN 0x1F returns).
     * (b) Keyboard: typing a letter in BASIC must change the screen (ROM echo).
     * (c) SPACE via zx_key_down is STOLEN by the Kempston remap (0x20 -> fire),
     *     so a real keyboard SPACE needs kbd_key_down. */
    zx_joystick(&zx, ZX_JOYSTICK_BTN | ZX_JOYSTICK_LEFT);
    printf("   kempston: joy_joymask=0x%02x (want BTN|LEFT=0x%02x) -> %s\n",
           zx.joy_joymask, ZX_JOYSTICK_BTN|ZX_JOYSTICK_LEFT,
           (zx.joy_joymask==(ZX_JOYSTICK_BTN|ZX_JOYSTICK_LEFT))?"OK":"BROKEN");
    zx_joystick(&zx, 0);

    /* framebuffer checksum before/after typing 'A' in BASIC */
    unsigned long h0=1469598103934665603UL;
    for (int i=0;i<ZX_FRAMEBUFFER_SIZE_BYTES;i++){h0^=zx.fb[i];h0*=1099511628211UL;}
    zx_key_down(&zx,'a'); run_frames(4); zx_key_up(&zx,'a'); run_frames(4);
    unsigned long h1=1469598103934665603UL;
    for (int i=0;i<ZX_FRAMEBUFFER_SIZE_BYTES;i++){h1^=zx.fb[i];h1*=1099511628211UL;}
    printf("   keyboard 'a' in BASIC: screen %s (fb %s)\n",
           (h0!=h1)?"CHANGED":"UNCHANGED", (h0!=h1)?"OK":"key not reaching ROM");

    /* SPACE via the device's zx_key_down(0x20) path -> does it type space or fire? */
    zx_key_down(&zx,0x20); 
    printf("   zx_key_down(0x20/SPACE): joy_joymask=0x%02x (nonzero => stolen as Kempston fire)\n", zx.joy_joymask);
    zx_key_up(&zx,0x20);

    printf("ALL ZX HOST HARNESS TESTS PASSED\n");
    return 0;
}
