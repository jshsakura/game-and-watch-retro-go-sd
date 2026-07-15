/* Sega/Mega CD system state — the CD hardware that sits on top of gwenesis.
 *
 * All writable CD RAM is resident (research: not pageable). Sizes are the real
 * hardware arrays; the linker overlay (.overlay_segacd_bss) places them across
 * AXI/AHB. See porting/segacd/CLAUDE.md for the RAM plan.
 */
#ifndef SEGACD_H
#define SEGACD_H

#include <stdint.h>
#include <stdio.h>   /* m68k.h uses FILE in its savestate prototypes */
#include "m68k.h"   /* m68ki_cpu_core — the sub-CPU context type */

#define SEGACD_PRG_RAM_SIZE   (512 * 1024)   /* sub-CPU program/work RAM */
#define SEGACD_WORD_RAM_SIZE  (256 * 1024)   /* shared graphics RAM (2M mode) */
#define SEGACD_PCM_RAM_SIZE   (64  * 1024)   /* RF5C164 waveform RAM */
#define SEGACD_BRAM_SIZE      (8   * 1024)   /* internal battery backup */
#define SEGACD_BIOS_SIZE      (128 * 1024)   /* region BIOS, XIP/read-only after load */

/* Gate-array register file (main side $A12000.., sub side $FF8000..). */
#define SEGACD_GA_REGS        0x200

/* RF5C164 PCM — 8 channels of 8-bit sample playback from pcm_ram. addr is a
 * fixed-point pointer into pcm_ram (PCM_STEP_SHIFT fractional bits). Modeled on
 * PicoDrive pd_cd/pcm.c; state lives here so savestate captures it. */
#define SEGACD_PCM_STEP_SHIFT 11
typedef struct {
    uint16_t env;            /* envelope (volume) */
    uint8_t  pan;            /* L/R pan nibbles */
    uint16_t fd;             /* frequency delta (step) */
    uint32_t addr;           /* fixed-point play pointer (<<PCM_STEP_SHIFT) */
    uint16_t start;          /* loop/start address (>>8 in reg) */
    uint16_t loop;           /* loop address */
} segacd_pcm_chan;

typedef struct {
    segacd_pcm_chan ch[8];
    uint8_t  control;        /* bit7 = sounding enable, low nibble = bank/cur-ch */
    uint8_t  enabled;        /* per-channel on/off mask */
    uint8_t  cur_ch;         /* channel selected for register writes */
    uint8_t  bank;           /* pcm_ram bank for the $FF window */
} segacd_pcm_t;

typedef struct {
    /* --- resident writable RAM (allocated to specific banks by the overlay) --- */
    uint8_t  *prg_ram;        /* SEGACD_PRG_RAM_SIZE  — AXI */
    uint8_t  *word_ram;       /* SEGACD_WORD_RAM_SIZE — AXI */
    uint8_t  *pcm_ram;        /* SEGACD_PCM_RAM_SIZE  — AHB */
    uint8_t   bram[SEGACD_BRAM_SIZE];

    /* --- gate array / control --- */
    uint8_t   s68k_regs[SEGACD_GA_REGS];   /* CDC/CDD/GA registers */
    uint8_t   prg_bank;                     /* main-CPU 128 KB window select (BK0/BK1) */
    uint8_t   word_mode;                    /* 0 = 2M, 1 = 1M/1M */
    uint8_t   word_owner;                   /* which CPU owns Word-RAM / active 1M bank */

    /* --- the sub-CPU: one m68ki_cpu_core context we swap in/out of the
     *     global `m68k` around each timeslice (see segacd_engine.c). --- */
    m68ki_cpu_core sub_ctx;
    int  sub_running;         /* cleared while sub is BUSREQ'd / reset-held */
    int  sub_idle;            /* set by poll/sleep fast-path (idle-skip lever) */

    segacd_pcm_t pcm;         /* RF5C164 8-channel PCM */

    /* --- CDC/CDD state lives in the adapted pd_cd/ layer --- */
} segacd_state;

extern segacd_state SCD;

/* engine (segacd_engine.c) */
void segacd_init(void);                 /* alloc RAM, reset both CPUs */
void segacd_reset(void);
int  segacd_run_sub(int cycle_target);  /* context-swap, run sub-68K, swap back */
void segacd_sub_release(void);          /* pulse-reset sub, start it (SRES clear) */
void segacd_sub_hold(void);             /* hold sub in reset / bus-request */

/* bus (segacd_bus.c) */
void segacd_sub_build_memory_map(void); /* fill SCD.sub_ctx.memory_map */
void segacd_main_map_cd_space(void);    /* patch main map: PRG win / Word / GA */
void segacd_map_bios(const uint8_t *bios); /* map region BIOS at main $000000 */

/* backup RAM (BRAM) persistence — 8 KB, per-game save file (segacd_cd.c) */
int  segacd_bram_load(const char *path);
int  segacd_bram_save(const char *path);

/* CD drive / controller / BIOS (segacd_cd.c) */
int  segacd_cd_open(const char *cue_path);
int  segacd_load_bios(const char *bios_path, uint8_t *dst, int max);
void segacd_cdd_process(void);          /* decode CDD command, update status */
void segacd_cdc_dma_sector(uint8_t *dst, int len);
void segacd_cd_update(void);            /* 75 Hz tick: pull next data sector */
int  segacd_cdda_fill(int16_t *dst, int frames);  /* stream CD-DA from SD */
int  segacd_cdda_prefetch(void);
void segacd_cdda_play(uint32_t lba);
void segacd_cdda_stop(void);

/* Audio: RF5C164 PCM + final mix (segacd_audio.c) */
void segacd_pcm_write(unsigned int reg, unsigned int val);
void segacd_pcm_update(int16_t *mixbuf, int frames);   /* stereo interleaved */
void segacd_audio_mix(int16_t *dst, const int16_t *ym, const int16_t *sn,
                      int frames, int volume);

#endif /* SEGACD_H */
