/* Sega/Mega CD — device porting layer, phase 5. Modeled 1:1 on
 * Core/Src/porting/pce/main_pce.c (app_main_pce): odroid_system_init +
 * emu_init(Load/Save/Screenshot/sleep/sram), then the frame loop, with the same
 * CD idioms — cue auto-start, per-frame CD tick, CD-DA prefetch in the sound
 * wait, magic-stamped savestate blocks for the CD RAM.
 *
 * Compiles as part of the full firmware build (needs APPID_SEGACD in appid.h and
 * the Makefile/linker-overlay wiring — the remaining mechanical phase-5 steps).
 * The base Mega Drive frame (main 68K + Z80 + VDP + YM/SN) reuses the gwenesis
 * core; this file adds the second 68K, the CDD tick and the CD audio mix.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "odroid_system.h"
#include "common.h"
#include "gw_lcd.h"
#include "rom_manager.h"
#include "appid.h"
#include "main.h"            /* hrtc — the crash checkpoint lives in a backup reg */
#include "error_screens.h"

#include "gwenesis_bus.h"
#include "gwenesis_vdp.h"
#include "gwenesis_savestate.h"
#include "gwenesis_io.h"
#include "gw_linker.h"
#include "gw_malloc.h"
#include "m68k.h"
#include "segacd.h"

/* base gwenesis frame (defined in the gwenesis porting layer / shared) */
void gwenesis_md_frame(bool draw_frame);   /* implemented below */
extern int16_t gwenesis_ym2612_buffer[];
extern int16_t gwenesis_sn76489_buffer[];

#define SEGACD_SAMPLE_RATE 53267   /* NTSC audio rate, matches gwenesis */

/* CDD ticks at a fixed 75Hz on real hardware (CD sector rate), independent of
 * the 50/60Hz video frame rate — NOT 1:1 with the video frame. Real hardware
 * and PicoDrive (pcd_cdc_event, cycle-scheduled) drive it at true 75Hz;
 * running it once per video frame instead gave 60Hz (NTSC) — 20% slow — which
 * widens the window MAIN has to write a new CDD command before the drive's
 * own next tick would have completed the previous one (PicoDrive's cmd=3/4
 * handlers are a 2-tick state-machine: tick 1 stages RS + arms a pending
 * flag and returns, tick 2 does the real seek — only if MAIN hasn't already
 * overwritten the command register in between). A slow tick rate gives MAIN
 * more real time per tick to intervene, which is exactly the failure mode
 * observed: MAIN rewrites the CDD command back to 0 one video frame (would
 * be <1 real CDD tick at true 75Hz) after issuing seek/play. */
extern int mode_pal;
static int s_cdd_tick_accum;

static char s_cue_path[512];
static char s_bram_path[540];
static int  s_cd_state_loaded;

/* Region BIOS as an external read-only pointer: the build wiring XIPs it from
 * flash (store_file_in_flash_relocate) and sets this, so BIOS costs 0 RAM.
 * Weak default NULL until that wiring lands (phase-5 finish). */
const uint8_t *segacd_bios __attribute__((weak)) = 0;

static void segacd_bram_path(void)
{
    snprintf(s_bram_path, sizeof(s_bram_path), "%s.brm", ACTIVE_FILE->path);
}

/* gwenesis_io.c (shared MD/32X/SegaCD engine) calls this on every emulated
 * joypad-port read to refresh button_state[] from the host gamepad — every
 * porting layer that uses the gwenesis engine must define it (see
 * Core/Src/porting/gwenesis/main_gwenesis.c for the MD original this is
 * ported from). segacd/ never had its own copy: the reference was silently
 * missing, the linker resolved it to MD's overlay (both overlays share the
 * same RAM_EMU VMA — see CLAUDE.md "Cores are overlays"), and on real
 * hardware MD's overlay is not loaded when SegaCD runs, so no button press
 * ever reached the emulated console. 0720 night 22 finding. */
void gwenesis_io_get_buttons()
{
    odroid_gamepad_state_t host_joystick;
    odroid_input_read_gamepad(&host_joystick);

    /* No dedicated third face button on the G&W pad; VOLUME is the least-bad
     * fixed default for Genesis C (MD's main_gwenesis.c instead offers a
     * runtime remap UI for this — out of scope here, add later if needed). */
    button_state[0] = host_joystick.values[ODROID_INPUT_UP]    << PAD_UP    |
                       host_joystick.values[ODROID_INPUT_DOWN]  << PAD_DOWN  |
                       host_joystick.values[ODROID_INPUT_LEFT]  << PAD_LEFT  |
                       host_joystick.values[ODROID_INPUT_RIGHT] << PAD_RIGHT |
                       host_joystick.values[ODROID_INPUT_A]     << PAD_A    |
                       host_joystick.values[ODROID_INPUT_B]     << PAD_B    |
                       host_joystick.values[ODROID_INPUT_VOLUME]<< PAD_C    |
                       host_joystick.values[ODROID_INPUT_START] << PAD_S;

    button_state[0] = ~button_state[0];
}

/* ---- savestate: base MD state + magic-stamped CD RAM (PCE pattern) ---- */
#define MAGIC_SCDR 0x53434452u   /* 'SCDR' : PRG/Word/PCM RAM + regs */
#define MAGIC_SCDD 0x53434444u   /* 'SCDD' : CDD position + PCM chan state */

static bool SaveState(const char *pathName)
{
    FILE *f = fopen(pathName, "wb");
    if (!f) return false;

    /* base Mega Drive state first (gwenesis savestate), then CD blocks. */
    extern void gwenesis_save_state(FILE *file);
    gwenesis_savestate_write_file_header(f);
    gwenesis_save_state(f);

    uint32_t tag = MAGIC_SCDR;
    fwrite(&tag, 4, 1, f);
    fwrite(SCD.prg_ram,  128 * 1024,  1, f);
    for (int i=128*1024; i<SEGACD_PRG_RAM_SIZE; i++) {
        uint8_t v = sub_prg_paged_read8(i);
        fwrite(&v, 1, 1, f);
    }
    fwrite(SCD.word_ram, SEGACD_WORD_RAM_SIZE, 1, f);
    fwrite(SCD.pcm_ram,  SEGACD_PCM_RAM_SIZE,  1, f);
    fwrite(SCD.s68k_regs, sizeof(SCD.s68k_regs), 1, f);
    fwrite(SCD.bram,      sizeof(SCD.bram),      1, f);

    tag = MAGIC_SCDD;
    fwrite(&tag, 4, 1, f);
    fwrite(&SCD.sub_ctx, sizeof(SCD.sub_ctx), 1, f);   /* sub-CPU registers */
    fwrite(&SCD.pcm,     sizeof(SCD.pcm),     1, f);
    fwrite(&SCD.prg_bank, 1, 1, f);
    fwrite(&SCD.word_mode, 1, 1, f);
    fwrite(&SCD.word_owner, 1, 1, f);
    fwrite(&SCD.sub_running, sizeof(int), 1, f);

    fclose(f);
    return true;
}

static bool LoadState(const char *pathName)
{
    FILE *f = fopen(pathName, "rb");
    if (!f) { s_cd_state_loaded = 0; return false; }

    unsigned char hdr[GWENESIS_SAVESTATE_HEADER_SIZE];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return false; }
    int ver = gwenesis_savestate_version_from_header(hdr);
    extern void gwenesis_load_state(FILE *file, int ss_version);
    gwenesis_load_state(f, ver);

    uint32_t tag = 0;
    /* CD RAM block — refuse anything not stamped by this build (research rule). */
    if (fread(&tag, 4, 1, f) == 1 && tag == MAGIC_SCDR) {
        fread(SCD.prg_ram,  128 * 1024,  1, f);
        for (int i=128*1024; i<SEGACD_PRG_RAM_SIZE; i++) {
            uint8_t v = 0;
            fread(&v, 1, 1, f);
            sub_prg_paged_write8(i, v);
        }
        fread(SCD.word_ram, SEGACD_WORD_RAM_SIZE, 1, f);
        fread(SCD.pcm_ram,  SEGACD_PCM_RAM_SIZE,  1, f);
        fread(SCD.s68k_regs, sizeof(SCD.s68k_regs), 1, f);
        fread(SCD.bram,      sizeof(SCD.bram),      1, f);
    }
    if (fread(&tag, 4, 1, f) == 1 && tag == MAGIC_SCDD) {
        fread(&SCD.sub_ctx, sizeof(SCD.sub_ctx), 1, f);
        fread(&SCD.pcm,     sizeof(SCD.pcm),     1, f);
        fread(&SCD.prg_bank, 1, 1, f);
        fread(&SCD.word_mode, 1, 1, f);
        fread(&SCD.word_owner, 1, 1, f);
        fread(&SCD.sub_running, sizeof(int), 1, f);
    }
    fclose(f);

    /* CRITICAL: SCD.sub_ctx.memory_map holds POINTERS into prg_ram/word_ram.
     * On a cold-boot load those allocations may sit at different addresses, so
     * the saved map is stale — rebuild it from the live RAM (registers/PC keep
     * their loaded values). Same rule that bit One Piece: load restores state,
     * not the pointer caches derived from it. Main map likewise re-applied. */
    segacd_sub_build_memory_map();
    segacd_main_map_cd_space();

    s_cd_state_loaded = 1;
    return true;
}

static void *Screenshot(void) { return NULL; }
static void sleep_wake_up(void) {}
static void sram_save_cb(void) { segacd_bram_save(s_bram_path); }

extern void blit(void);   /* provided by the gwenesis blit path */
extern uint8_t *odroid_overlay_cache_file_in_flash_relocate(const char *file_path, uint32_t *file_size_p, bool byte_swap, void (*relocate_cb)(uint8_t *, uint32_t, uint32_t, uint8_t *, uint32_t));
extern uint8_t *odroid_overlay_cache_file_in_flash(const char *file_path, uint32_t *file_size_p, bool byte_swap);

#define SEGACD_CODE_BASE  0xDEC80000u
#define SEGACD_XIP_PATH   "/cores/segacd.xip"

static uint8_t *g_xip_addr;
static uint32_t g_xip_size;
static int32_t  g_xip_offset;

static int PatchSegaCdSentinels(uint32_t *start, uint32_t *end, int32_t offset, uint32_t size) {
  int patched = 0;
  for (uint32_t *p = start; p < end; p++) {
    uint32_t v = *p;
    if ((v & ~1u) >= SEGACD_CODE_BASE && (v & ~1u) < SEGACD_CODE_BASE + size) {
      *p = (uint32_t)(v + offset);
      patched++;
    }
  }
  return patched;
}

static void SegaCdRelocateXip(uint8_t *buffer, uint32_t length, uint32_t offset_in_file,
                              uint8_t *file_address, uint32_t file_size) {
  (void)offset_in_file;
  int32_t offset = (int32_t)((uint32_t)file_address - SEGACD_CODE_BASE);
  PatchSegaCdSentinels((uint32_t *)buffer, (uint32_t *)(buffer + (length & ~3u)), offset, file_size);
}

/* ---- crash checkpoint --------------------------------------------------
 *
 * The device symptom has been the same sentence since 0721: no screen, no
 * BSOD, no log, back at the launcher. That one observation covers at least
 * five different faults — a missing core blob, a missing BIOS, a hang before
 * the frame loop, a hang inside it, and a watchdog reset — and nothing in this
 * core distinguishes them, which is why "Sega CD dies on hardware" has stayed
 * a symptom instead of becoming a diagnosis. printf() goes down a UART nobody
 * has, and writing to the SD during play is forbidden (FAT corruption), so the
 * evidence has to survive a chip reset somewhere else: an RTC backup register.
 *
 * DR27 is free. DR0 is the OFW boot flag, DR1 the alarm epoch, DR28 the
 * boot-rescue counter, DR29 the clock snapshot (rg_rtc.c CLOCK_BKP_REG) and
 * DR30 the charger — an adversarial review proposed DR29 for exactly this and
 * would have wiped the player's clock once per frame. */
#define SEGACD_BKP_REG    RTC_BKP_DR27
#define SEGACD_CP_MAGIC   0x5CD00000u
#define SEGACD_CP_MASK    0xFFFF0000u
#define SEGACD_CP_FRAMED  0x8000u          /* low bits are a frame count */

enum {
    SCD_CP_NONE = 0, SCD_CP_ENTER, SCD_CP_XIP, SCD_CP_BIOS, SCD_CP_INIT,
    SCD_CP_MAPBIOS, SCD_CP_RESET, SCD_CP_CD, SCD_CP_BRAM, SCD_CP_LOOP,
    SCD_CP_DONE = 0x7FFF
};

static const char *scd_checkpoint_name(uint32_t cp) {
    if (cp & SEGACD_CP_FRAMED) return "inside the frame loop";
    switch (cp) {
        case SCD_CP_ENTER:   return "entry, before caching the core";
        case SCD_CP_XIP:     return "caching segacd.xip into flash";
        case SCD_CP_BIOS:    return "caching the BIOS into flash";
        case SCD_CP_INIT:    return "power_on / segacd_init";
        case SCD_CP_MAPBIOS: return "segacd_map_bios";
        case SCD_CP_RESET:   return "reset_emulation";
        case SCD_CP_CD:      return "segacd_cd_open";
        case SCD_CP_BRAM:    return "loading BRAM";
        case SCD_CP_LOOP:    return "first frame";
        default:             return "unknown stage";
    }
}

static void scd_checkpoint(uint32_t cp) {
    HAL_RTCEx_BKUPWrite(&hrtc, SEGACD_BKP_REG, SEGACD_CP_MAGIC | (cp & 0xFFFFu));
}

/* Whatever the PREVIOUS run of this core reached, or SCD_CP_NONE if there is
 * nothing worth reporting. Read once, at entry, before we overwrite it.
 *
 * Leaving the core through the in-game menu does not unwind this function, so
 * a normal session also ends on a frame checkpoint. Reporting that would put a
 * fault screen in front of the player every single launch, which is how a
 * diagnostic gets switched off. So: any stage BEFORE the frame loop is always
 * a fault (this core has never once reached the loop on hardware), and a frame
 * checkpoint is only a fault if it died almost immediately. Nobody plays for
 * three seconds on purpose; everybody who hits the current bug dies inside
 * one. */
#define SEGACD_CP_ALIVE_FRAMES 180u        /* ~3 s at 60 fps */

static uint32_t scd_checkpoint_previous(void) {
    uint32_t v = HAL_RTCEx_BKUPRead(&hrtc, SEGACD_BKP_REG);
    if ((v & SEGACD_CP_MASK) != SEGACD_CP_MAGIC) return SCD_CP_NONE;
    uint32_t cp = v & 0xFFFFu;
    if (cp == SCD_CP_DONE) return SCD_CP_NONE;
    if ((cp & SEGACD_CP_FRAMED) &&
        (cp & ~SEGACD_CP_FRAMED) >= SEGACD_CP_ALIVE_FRAMES)
        return SCD_CP_NONE;                /* it ran; the player just left */
    return cp;
}

/* Say it on the LCD and hold it there — a message the launcher repaints over
 * before it can be read is the same as no message at all (main_sm.c:139). */
static void segacd_hold_screen(const char *main_line, const char *l1, const char *l2) {
    printf("segacd: %s / %s / %s\n", main_line ? main_line : "",
           l1 ? l1 : "", l2 ? l2 : "");
    lcd_backlight_set(180);
    draw_error_screen(main_line, l1, l2);
    odroid_gamepad_state_t joy;
    do {                                   /* let go of the launch press first */
        wdog_refresh(); lcd_sync(); lcd_swap();
        odroid_input_read_gamepad(&joy);
        HAL_Delay(20);
    } while (joy.bitmask != 0);
    do {
        wdog_refresh(); lcd_sync(); lcd_swap();
        odroid_input_read_gamepad(&joy);
        HAL_Delay(20);
    } while (joy.bitmask == 0);
}

static bool SegaCdCacheXipToFlash(void) {
  g_xip_size = 0;
  g_xip_addr = odroid_overlay_cache_file_in_flash_relocate(SEGACD_XIP_PATH, &g_xip_size, false,
                                                           &SegaCdRelocateXip);
  if (g_xip_addr == NULL || g_xip_size == 0) {
    printf("segacd: %s missing\n", SEGACD_XIP_PATH);
    return false;
  }
  g_xip_offset = (int32_t)((uint32_t)g_xip_addr - SEGACD_CODE_BASE);
  printf("segacd: xip blob at %p, %lu bytes, offset 0x%08lX\n",
         g_xip_addr, (unsigned long)g_xip_size, (unsigned long)g_xip_offset);
  
  PatchSegaCdSentinels((uint32_t *)__RAM_EMU_START__, (uint32_t *)__RAM_EMU_END__, g_xip_offset, g_xip_size);
  return true;
}

/* ---- entry point (called by the launcher, like app_main_pce) ---- */
int app_main_segacd(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    if (start_paused) { common_emu_state.pause_after_frames = 2; odroid_audio_mute(true); }
    else              { common_emu_state.pause_after_frames = 0; }

    odroid_system_init(APPID_SEGACD, SEGACD_SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, &Screenshot, NULL, &sleep_wake_up, &sram_save_cb);

    /* Every core that calls ram_malloc() for its own buffers must point
     * ram_start past its own static overlay+bss first (see main_gwenesis.c's
     * ram_start = &_OVERLAY_MD_BSS_END for the pattern this copies) — this was
     * missing here, so SCD.prg_ram/word_ram (ram_malloc, below) would have
     * started allocating at RAM_EMU's very start, colliding with this core's
     * own overlay code+data+bss instead of the free space after it. 0720. */
    ram_start = (uint32_t)&_OVERLAY_SEGACD_BSS_END;

    /* Report where the last run stopped BEFORE overwriting the record. A
     * watchdog reset takes the whole chip down without unwinding, so this is
     * the only witness that survives it. */
    {
        uint32_t prev = scd_checkpoint_previous();
        if (prev != SCD_CP_NONE) {
            char line[64];
            if (prev & SEGACD_CP_FRAMED)
                snprintf(line, sizeof(line), "after %lu frames",
                         (unsigned long)(prev & ~SEGACD_CP_FRAMED));
            else
                snprintf(line, sizeof(line), "stage %lu", (unsigned long)prev);
            segacd_hold_screen("Sega CD stopped last run", scd_checkpoint_name(prev), line);
        }
    }
    scd_checkpoint(SCD_CP_ENTER);

    scd_checkpoint(SCD_CP_XIP);
    if (!SegaCdCacheXipToFlash()) {
        printf("Failed to cache segacd.xip\n");
        segacd_hold_screen("Sega CD core file missing", SEGACD_XIP_PATH,
                           "Copy the cores folder onto the SD card");
        scd_checkpoint(SCD_CP_DONE);
        return 0;
    }

    uint32_t bios_size = 0;
    scd_checkpoint(SCD_CP_BIOS);
    segacd_bios = odroid_overlay_cache_file_in_flash("/bios/segacd/bios_CD_U.bin", &bios_size, false);
    if (!segacd_bios) {
        segacd_bios = odroid_overlay_cache_file_in_flash("/bios/segacd/bios_CD_E.bin", &bios_size, false);
    }
    if (!segacd_bios) {
        segacd_bios = odroid_overlay_cache_file_in_flash("/bios/segacd/bios_CD_J.bin", &bios_size, false);
    }
    if (!segacd_bios) {
        printf("SegaCD BIOS not found in /bios/segacd/!\n");
        segacd_hold_screen("Sega CD BIOS missing", "/bios/segacd/bios_CD_U|E|J.bin",
                           "Put one of the three BIOS files there");
        scd_checkpoint(SCD_CP_DONE);
        return 0;
    }

    /* base Mega Drive core + Sega CD hardware.
     *
     * ORDER IS LOAD-BEARING. reset_emulation() calls m68k_pulse_reset(), which
     * immediately fetches the MAIN 68K's initial SP/PC from $000000/$000004
     * through the memory map. On a Mega CD the main 68K boots from the *BIOS*,
     * not from ROM_DATA — and on device ROM_DATA is the CD image, not the BIOS
     * (unlike the host harness, which aliases ROM_DATA = bios and so gets away
     * with resetting earlier). So the reset MUST run last, after:
     *   - power_on()               builds the base gwenesis memory map (page 0 = ROM_DATA)
     *   - segacd_map_bios()        overrides page 0-1 with the BIOS
     *   - segacd_main_map_cd_space() overlays PRG/GA/Word-RAM
     * Only then does the vector fetch land in the BIOS. The previous order
     * (reset before power_on) fetched vectors from an unbuilt map, leaving the
     * MAIN 68K running from a garbage PC — a device-only boot Hardfault
     * (m68ki_read_8 dispatching through a wild page handler). power_on() already
     * calls m68k_init(), so no separate call is needed. */
    load_cartridge();
    scd_checkpoint(SCD_CP_INIT);
    power_on();
    segacd_init();
    scd_checkpoint(SCD_CP_MAPBIOS);
    segacd_map_bios(segacd_bios); /* main boots from BIOS, not a cart (0 RAM: XIP) */
    segacd_main_map_cd_space();
    scd_checkpoint(SCD_CP_RESET);
    reset_emulation();            /* pulse-reset MAIN 68K -> reads BIOS reset vectors */

    scd_checkpoint(SCD_CP_CD);
    snprintf(s_cue_path, sizeof(s_cue_path), "%s", ACTIVE_FILE->path);
    segacd_cd_open(s_cue_path);

    scd_checkpoint(SCD_CP_BRAM);
    segacd_bram_path();
    segacd_bram_load(s_bram_path);   /* per-game BRAM, load before resume */

    if (load_state) odroid_system_emu_load_state(save_slot);
    else            lcd_clear_buffers();

    odroid_gamepad_state_t joystick = {0};
    odroid_dialog_choice_t options[] = { ODROID_DIALOG_CHOICE_LAST };

    /* sub_cycles_per_frame removed as it's interleaved inside gwenesis_md_frame */

    scd_checkpoint(SCD_CP_LOOP);

    uint32_t scd_frames = 0;
    while (true) {
        wdog_refresh();
        /* One backup-register write per frame. If the watchdog takes the chip
         * down mid-frame, the next run says how far it got: "hung on frame 1"
         * and "ran 400 frames then hung" are different bugs. */
        scd_checkpoint(SEGACD_CP_FRAMED | (++scd_frames & 0x7FFFu));
        bool drawFrame = common_emu_frame_loop();

        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, &blit);

        /* --- one frame of the machine --- */
        gwenesis_md_frame(drawFrame);           /* main 68K + Z80 + VDP + YM/SN + Sub 68K interleaved */
        /* True 75Hz CDD pacing (see comment above s_cdd_tick_accum) — average
         * 1.25 ticks/frame at NTSC 60fps, 1.5 ticks/frame at PAL 50fps. */
        s_cdd_tick_accum += 75;
        int video_fps = mode_pal ? 50 : 60;
        while (s_cdd_tick_accum >= video_fps) {
            segacd_cdd_process();
            segacd_cd_update();
            segacd_cdc_dma_update();
            s_cdd_tick_accum -= video_fps;
        }

        if (drawFrame) blit();

        /* --- audio: gwenesis YM+SN mixed with CD-DA + RF5C164 PCM --- */
        int16_t *sbuf = audio_get_active_buffer();
        uint16_t slen = audio_get_buffer_length();
        int vol = common_emu_sound_get_volume();
        segacd_audio_mix(sbuf, gwenesis_ym2612_buffer, gwenesis_sn76489_buffer, slen, vol);
        odroid_audio_submit(sbuf, slen);

        common_emu_sound_sync(false);
        segacd_cdda_prefetch();      /* keep the CD-DA stream fed (PCE idiom) */
    }
    return 0;
}

int scd_dbg_frame = 0;

/* --- Gwenesis core internals for frame rendering --- */
extern unsigned short gwenesis_vdp_status;
extern unsigned char gwenesis_vdp_regs[32];

/* REG macros and STATUS macros are defined in gwenesis_vdp.h */
#define LINES_PER_FRAME_NTSC 262
#define LINES_PER_FRAME_PAL 313
#define VDP_CYCLES_PER_LINE 3420

extern unsigned int screen_height, screen_width;
extern int system_clock, zclk, ym2612_clock, ym2612_index, sn76489_clock, sn76489_index, scan_line;
extern int hint_pending;
int hint_counter = 0, skip_first_vint = 0;

extern void m68k_run(unsigned int target);
extern void z80_run(unsigned int target);
extern void z80_irq_line(int state);
extern void gwenesis_SN76489_run(unsigned int target);
extern void ym2612_run(unsigned int target);
extern void gwenesis_vdp_render_line(int line);
extern void gwenesis_vdp_set_buffer(unsigned short *ptr_screen_buffer);
extern void gwenesis_vdp_render_config(void);
extern void gwenesis_vdp_latch_line_scroll(int line);
extern void m68k_set_irq(unsigned int level);
extern void m68k_update_irq(unsigned int level);
extern void gw_system_blit(void *buffer);

static inline void run_main(uint32_t target) { m68k_run(target); }
static inline void run_z80(uint32_t target) { z80_run(target); }
static inline void run_audio(uint32_t target) { gwenesis_SN76489_run(target); ym2612_run(target); }
static inline void run_sub(int slice) { segacd_run_sub(slice); }
static inline void render_line(int line, bool draw) { if (draw) gwenesis_vdp_render_line(line); }

void gwenesis_md_frame(bool draw_frame) {
    screen_height = REG1_PAL?240:224; screen_width = REG12_MODE_H40?320:256;
    unsigned int lines_per_frame = mode_pal?LINES_PER_FRAME_PAL:LINES_PER_FRAME_NTSC;
    int vert_screen_offset = mode_pal?0:320*(240-224)/2;
    uint16_t *fb = (uint16_t *)lcd_get_active_buffer();
    gwenesis_vdp_set_buffer(&fb[vert_screen_offset]); gwenesis_vdp_render_config();
    system_clock=0; zclk=0; ym2612_clock=ym2612_index=0; sn76489_clock=sn76489_index=0; scan_line=0;
    int line;
    int sub_slice = (int)((12500000 / 60) / lines_per_frame); /* 12.5 MHz / 60 fps / lines */
    
    gwenesis_vdp_status=(unsigned short)((gwenesis_vdp_status&(unsigned short)~0x0112u)|STATUS_VBLANK);
    gwenesis_vdp_status^=STATUS_ODDFRAME;
    scan_line=(int)screen_height;
    if(!skip_first_vint){ gwenesis_vdp_status|=STATUS_VIRQPENDING;
      if(REG1_VBLANK_INTERRUPT){m68k_set_irq(6);} z80_irq_line(1); }
    run_main(system_clock+VDP_CYCLES_PER_LINE); run_z80(system_clock+VDP_CYCLES_PER_LINE);
    system_clock+=VDP_CYCLES_PER_LINE; z80_irq_line(0);
    
    for(line=(int)screen_height+1; line<(int)lines_per_frame-1; line++){ 
      scan_line=line;
      run_main(system_clock+VDP_CYCLES_PER_LINE); run_z80(system_clock+VDP_CYCLES_PER_LINE); system_clock+=VDP_CYCLES_PER_LINE;
      run_sub(sub_slice); 
    }
    
    scan_line=(int)lines_per_frame-1; hint_counter=(int)REG10_LINE_COUNTER;
    gwenesis_vdp_status&=(unsigned short)~STATUS_VBLANK;
    run_main(system_clock+VDP_CYCLES_PER_LINE); run_z80(system_clock+VDP_CYCLES_PER_LINE); system_clock+=VDP_CYCLES_PER_LINE;
    
    for(line=0; line<(int)screen_height; line++){ 
      scan_line=line; gwenesis_vdp_latch_line_scroll(line);
      if(hint_counter==0){hint_counter=(int)REG10_LINE_COUNTER; hint_pending=1; if(REG0_LINE_INTERRUPT)m68k_update_irq(4);} else hint_counter--;
      run_main(system_clock+VDP_CYCLES_PER_LINE); run_z80(system_clock+VDP_CYCLES_PER_LINE);
      render_line(line, draw_frame); system_clock+=VDP_CYCLES_PER_LINE;
      run_sub(sub_slice); 
    }
    run_audio(system_clock); 
    /* Adjust m68k.cycles by subtracting system_clock? boot_test does this: m68k.cycles-=system_clock;
       But we need to access m68k struct. I'll declare it: */
    extern m68ki_cpu_core m68k;
    m68k.cycles -= system_clock;
    skip_first_vint=0;
}

extern void common_ingame_overlay(void);

void blit(void) {
    uint16_t *fb = (uint16_t *)lcd_get_active_buffer();
    int vert_offset = mode_pal ? 0 : 320 * (240 - 224) / 2;
    gwenesis_vdp_set_buffer(&fb[vert_offset]);
    int lines = mode_pal ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
    for (int l = 0; l < lines; l++) {
        gwenesis_vdp_render_line(l);
    }
    common_ingame_overlay();
}
