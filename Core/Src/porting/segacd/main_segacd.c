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

#include "gwenesis_bus.h"
#include "gwenesis_vdp.h"
#include "m68k.h"
#include "segacd.h"

/* base gwenesis frame (defined in the gwenesis porting layer / shared) */
extern void gwenesis_md_frame(bool draw_frame);   /* runs main 68K+Z80+VDP one frame */
extern int16_t gwenesis_ym2612_buffer[];
extern int16_t gwenesis_sn76489_buffer[];

#define SEGACD_SAMPLE_RATE 53267   /* NTSC audio rate, matches gwenesis */
#define CDD_TICKS_PER_FRAME 1      /* 75 Hz CDD vs ~60 fps → ~1.25 ticks/frame */

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

/* ---- savestate: base MD state + magic-stamped CD RAM (PCE pattern) ---- */
#define MAGIC_SCDR 0x53434452u   /* 'SCDR' : PRG/Word/PCM RAM + regs */
#define MAGIC_SCDD 0x53434444u   /* 'SCDD' : CDD position + PCM chan state */

static bool SaveState(char *pathName)
{
    FILE *f = fopen(pathName, "wb");
    if (!f) return false;

    /* base Mega Drive state first (gwenesis savestate), then CD blocks. */
    extern void gwenesis_save_state(FILE *file);
    gwenesis_savestate_write_file_header(f);
    gwenesis_save_state(f);

    uint32_t tag = MAGIC_SCDR;
    fwrite(&tag, 4, 1, f);
    fwrite(SCD.prg_ram,  SEGACD_PRG_RAM_SIZE,  1, f);
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

static bool LoadState(char *pathName)
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
        fread(SCD.prg_ram,  SEGACD_PRG_RAM_SIZE,  1, f);
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

static bool Screenshot(const char *name, int width, int height) { (void)name; (void)width; (void)height; return false; }
static void sleep_wake_up(void) {}
static void sram_save_cb(void) { segacd_bram_save(s_bram_path); }

extern void blit(void);   /* provided by the gwenesis blit path */

/* ---- entry point (called by the launcher, like app_main_pce) ---- */
int app_main_segacd(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    if (start_paused) { common_emu_state.pause_after_frames = 2; odroid_audio_mute(true); }
    else              { common_emu_state.pause_after_frames = 0; }

    odroid_system_init(APPID_SEGACD, SEGACD_SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, &Screenshot, NULL, &sleep_wake_up, &sram_save_cb);

    /* base Mega Drive core + Sega CD hardware */
    load_cartridge();
    m68k_init();
    reset_emulation();
    power_on();
    segacd_init();
    segacd_map_bios(segacd_bios); /* main boots from BIOS, not a cart (0 RAM: XIP) */
    segacd_main_map_cd_space();

    snprintf(s_cue_path, sizeof(s_cue_path), "%s", ACTIVE_FILE->path);
    segacd_cd_open(s_cue_path);

    segacd_bram_path();
    segacd_bram_load(s_bram_path);   /* per-game BRAM, load before resume */

    if (load_state) odroid_system_emu_load_state(save_slot);
    else            lcd_clear_buffers();

    odroid_gamepad_state_t joystick = {0};
    odroid_dialog_choice_t options[] = { ODROID_DIALOG_CHOICE_LAST };

    const int sub_cycles_per_frame = 12500000 / 60;   /* sub-68K 12.5 MHz @ 60 fps */

    while (true) {
        wdog_refresh();
        bool drawFrame = common_emu_frame_loop();

        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, &blit);

        /* --- one frame of the machine --- */
        gwenesis_md_frame(drawFrame);           /* main 68K + Z80 + VDP + YM/SN */
        segacd_run_sub(sub_cycles_per_frame);   /* sub 68K, context-swapped */
        for (int t = 0; t < CDD_TICKS_PER_FRAME; t++) {
            segacd_cdd_process();
            segacd_cd_update();
            segacd_cdc_dma_update();
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
