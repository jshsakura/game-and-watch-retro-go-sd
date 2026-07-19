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

#define SEGACD_PRG_RAM_SIZE   (128 * 1024)   /* sub-CPU program/work RAM (paged) */
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

    /* Word-RAM DMNA/RET handshake shadow (2M mode), mirroring PicoDrive's
     * dmna_ret_2m (pd_cd/memory.c): bit0 = RET, bit1 = DMNA. This is the
     * persistent source of truth for $A12003/$FF8003 bits 0-1 — every MAIN
     * or SUB write to that register folds the CURRENT value of this shadow
     * into the bits it doesn't own (see main_ga_write8/sub_ff_write8 reg==3
     * in segacd_bus.c). Reset state is RET=1 (main owns Word-RAM). */
    uint8_t   dmna_ret_2m;

    /* --- the sub-CPU: one m68ki_cpu_core context we swap in/out of the
     *     global `m68k` around each timeslice (see segacd_engine.c). --- */
    m68ki_cpu_core sub_ctx;
    int  sub_running;         /* cleared while sub is BUSREQ'd / reset-held */
    int  sub_idle;            /* set by poll/sleep fast-path (idle-skip lever) */

    /* ---- idle-skip (THE top speed lever) ----
     * The sub-68K spends most cycles spinning on a gate-array status register
     * waiting for the main CPU / CDD. When it reads the same GA reg in a tight
     * loop with no intervening write, we mark it idle and skip its remaining
     * timeslice; any write that could change what it polls re-arms it. Same
     * lever proven on GBA/VB/WonderSwan here. */
    uint8_t  poll_reg;        /* last GA reg the sub read */
    uint16_t poll_count;      /* consecutive unchanged reads of poll_reg */
    uint32_t poll_clk;        /* abs sub cycle count of last poll read */
    uint32_t sub_cycle_accum; /* abs sub cycle accumulator (survives rebase) */

    int  cdd_int_pending;     /* CDD level-4 interrupt (periodic status export) armed */
    int  cdc_int_pending;     /* CDC level-5 interrupt (DECI/DTEI) armed — segacd_cd.c */

    /* Word-RAM graphics-transform ASIC completion interrupt (level 1, "GFX").
     * The real gate array renders a rotated/scaled image into Word-RAM after
     * the sub triggers an op ($FF8066 write) and asserts INT1 when the line
     * counter runs out, gated by IEN1 ($FF8033 bit1). The sub's BIOS boot
     * animation loop (sub PC 0x7a06) blocks on the frame-counter toggle its
     * INT1 handler (0x7ace) produces, so without this the boot logo never
     * advances. Full stamp/trace rendering (pd_cd/gfx.c) is TODO; for now we
     * model only the completion interrupt so the sub's state machine runs.
     * gfx_op_armed is set when the op is triggered and promoted to a
     * deliverable gfx_int_pending once per frame (frame-paced, mirroring the
     * ~1-frame ASIC latency the animation is timed against). */
    int  gfx_op_armed;
    int  gfx_int_pending;

    /* MAIN->SUB "IFL2" doorbell — mirrors real hardware's $A12000 bit0 (main
     * side) / the sub's level-2 IRQ input. Set by main's write; consumed
     * (cleared to 0) the instant the sub's level-2 interrupt is actually
     * delivered — a ONE-SHOT pulse, not a held level. This was tested and
     * confirmed against PicoDrive's own Musashi ack callback (pd_cd/sek.c
     * SekIntAckMS68k -> new_irq_level(2) does `state_flags &=
     * ~PCD_ST_S68K_IFL2` on ACK, unconditionally) before trusting it — see
     * segacd_run_sub's delivery loop for the "why one-shot, not level"
     * writeup and why one delivery per frame wasn't always enough anyway
     * (CDD/level-4 can legitimately win several slots in the same frame
     * ahead of it).
     *
     * Kept as its OWN field, separate from SCD.s68k_regs[0], because $A12000
     * (main view) and $FF8000 (sub view: gate-array version + LED bits) are
     * genuinely DIFFERENT registers on real hardware that happen to share
     * offset 0 from each CPU's own base — routing both through the same
     * shared regs[0] byte aliases the sub's frequent LED-status writes onto
     * what main reads back as the doorbell flag. See segacd_bus.c
     * main_ga_read8/write8 reg 0. */
    uint8_t  ga_ifl2;

    /* MAIN's SRES/SBRQ control shadow — mirrors PicoDrive's Pico_mcd->m.busreq
     * (pd_cd/memory.c m68k_reg_write8 case 1: `Pico_mcd->m.busreq = d; return;`
     * — note it does NOT touch s68k_regs[1] at all). bit0 = SRES (0 = sub
     * held in reset, 1 = running), bit1 = SBRQ (1 = main holds the sub bus).
     *
     * Kept as its OWN field, separate from SCD.s68k_regs[1], for the exact
     * reason documented on ga_ifl2 above: $A12001 (main's reset/busreq
     * control) and $FF8001 (sub's own LED/soft-reset-trigger register,
     * pd_cd/memory.c s68k_reg_write8 case 1: `if (!(d&1)) pcd_soft_reset();
     * return;` — no persistent store either) are genuinely DIFFERENT
     * registers that happen to share offset 1 from each CPU's own base.
     * Routing both through the shared regs[1] byte let a SUB write to its
     * own $FF8001 (segacd_bus.c sub_ff_write8's generic default case)
     * silently clobber MAIN's SRES=1/SBRQ=0 state — main's 0x1252-style
     * "send next CDD command" routine gates on reading back exactly that
     * bit, so the clobber permanently blocked every doorbell after the
     * first. See segacd_bus.c main_ga_read8/write8 reg 1. */
    uint8_t  main_busreq;

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

/* Rebuild both CPUs' Word-RAM mapping (.base pointers only) from the CURRENT
 * SCD.word_mode / SCD.s68k_regs[0x03] bit0 state — 2M mode: one shared 256KB
 * block; 1M mode: two independent 128KB banks, swapped by bit0, so main and
 * sub never alias the same physical bytes. Call after ANY write that changes
 * reg3 bits 0/2 (from either CPU's write handler in segacd_bus.c) and once at
 * init. `called_from_sub` must say which CPU context is CURRENTLY active in
 * the shared `m68k` global (segacd_engine.c's s_main_saved swap shadow only
 * holds a valid main snapshot while sub is active) — get this wrong and the
 * remap corrupts the wrong CPU's memory map. */
void segacd_word_ram_remap(int called_from_sub);

/* bus (segacd_bus.c) */
void segacd_sub_build_memory_map(void); /* fill SCD.sub_ctx.memory_map */
void segacd_main_map_cd_space(void);    /* patch main map: PRG win / Word / GA */
void segacd_map_bios(const uint8_t *bios); /* map region BIOS at main $000000 */
void segacd_poll_wake(void);            /* re-arm the sub after a GA/CDD change */

/* backup RAM (BRAM) persistence — 8 KB, per-game save file (segacd_cd.c) */
int  segacd_bram_load(const char *path);
int  segacd_bram_save(const char *path);

/* CD drive / controller / BIOS (segacd_cd.c) */
int  segacd_cd_open(const char *cue_path);
int  segacd_load_bios(const char *bios_path, uint8_t *dst, int max);

unsigned int sub_prg_paged_read8(unsigned int address);
void sub_prg_paged_write8(unsigned int address, unsigned int data);
void segacd_cdd_process(void);          /* 75 Hz tick: latency/export cadence */
void segacd_cdd_command(void);          /* decode+respond to a 10-byte CDD command */
void segacd_cdc_dma_sector(uint8_t *dst, int len);
void segacd_cd_update(void);            /* 75 Hz tick: drive mechanics (lba/track) */

/* CDC (data controller, LC89510-compatible) — segacd_cd.c. Register-index
 * protocol at $FF8005 (index)/$FF8007 (data); host data port at
 * $A12008 (main)/$FF8008 (sub). See segacd_cd.c's CDC section for the
 * behavioral reference (pd_cd/cdc.c) and what is deliberately not modeled
 * (PRG/Word/PCM DMA destinations). */
void     segacd_cdc_reset(void);
void     segacd_cdc_reg_w(uint8_t data);
uint8_t  segacd_cdc_reg_r(void);
uint16_t segacd_cdc_host_r(int sub);
void     segacd_cdc_dma_update(void);     /* one-shot PRG/PCM/Word-RAM DMA (DTRG=4/5/7) */
void     segacd_subcode_q_update(void);   /* synthesize subchannel Q → $FF8100 + $FF8069 */
/* Word-RAM graphics-transform ASIC (segacd_gfx.c, ported from pd_cd/gfx.c).
 * segacd_gfx_start() renders the rotated/scaled image into Word-RAM in one shot
 * on a $FF8066 (start) write and returns 1 if an op ran (arm level-1 INT1). */
void segacd_gfx_init(void);
int  segacd_gfx_start(uint32_t base);

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
