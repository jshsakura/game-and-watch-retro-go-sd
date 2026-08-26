#ifndef GW_CRASH_CRUMBS_H
#define GW_CRASH_CRUMBS_H

/* Crash breadcrumbs in RTC backup registers DR2-DR11.
 *
 * The rare mid-run crash (ratio 1.5160 anomaly, 2026-08-25) recovers via
 * launcher reboot and leaves every fault register at zero -- nothing to
 * diagnose after the fact. These crumbs make the death observable: the
 * fault handler and the WWDG early-wakeup interrupt record the last
 * PC/LR/counters into the backup domain, and the next boot copies them
 * out into g_last_crumb for SWD reading before clearing.
 *
 * Register map (DR0 OFW flag, DR1 alarm, DR28 boot-fail, DR29 clock,
 * DR30 charger are owned elsewhere and never touched here):
 *   DR2  magic  0xC4AB0001 -- distinguishes crumbs from DR28 0xB007'000N
 *   DR3  cause  1 = fault handler, 2 = WWDG early wakeup
 *   DR4  PC     (fault path only; 0 for WWDG -- HAL callback context)
 *   DR5  LR     (fault path only)
 *   DR6  CFSR   snapshot at death
 *   DR7  HFSR   snapshot at death
 *   DR8  emu frame counter, heartbeat-refreshed (~1 Hz)
 *   DR9  drawn frame counter, heartbeat-refreshed
 *   DR10 RCC_RSR reset-cause snapshot of the *previous* boot, read at boot
 *   DR11 heartbeat sequence number (parity-of-life for the DR8/DR9 pair)
 *   DR12 LIVE modal state -- nonzero while a modal loop owns the core,
 *       cleared on modal exit, so SWD can name the state without a reboot
 *   DR13 gamepad bitmask the entry poll saw (0 = no input involved)
 *   DR14 RTC->TR (BCD) at modal entry -- cross-check against wall clock
 */

#include <stdint.h>

typedef struct {
    uint32_t magic;
    uint32_t cause;      /* 0 = none recorded since last boot read */
    uint32_t pc;
    uint32_t lr;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t emu;
    uint32_t drawn;
    uint32_t rsr_prev;   /* RCC_RSR of the boot that just ended */
    uint32_t modal;      /* DR12 code of the last modal entered before death */
    uint32_t modal_input;/* DR13 gamepad bitmask at that entry */
    uint32_t modal_tr;   /* DR14 RTC->TR (BCD) at that entry */
    uint32_t wdog_armed; /* DR15: bit0 clock, bit1 WDGA, bit2 retried */
} gw_crumb_t;

/* Modal codes: the code IS the entry path, so 'did a button poll see a
 * value' is answered by the code itself. 3 can only be reached through a
 * PAUSE/SET poll that saw the button; 2 needs an app restart (no reboot =
 * mid-run code 2 is a defect); 1 is poll-driven; 4/5 are the power paths. */
#define CRUMB_MODAL_ALARM      1u  /* in-game alarm ring; auto-dismisses 60 s */
#define CRUMB_MODAL_RESUME     2u  /* PAUSE banner via pause_after_frames */
#define CRUMB_MODAL_MENU       3u  /* settings menu, PAUSE/SET release path */
#define CRUMB_MODAL_SLEEPMENU  4u  /* PAUSE/SET + POWER combo path */
#define CRUMB_MODAL_SLEEP      5u  /* POWER-only save-state-and-poweroff path */

/* Populated by gw_crumb_boot() on every boot; read over SWD. */
extern gw_crumb_t g_last_crumb;

void gw_crumb_boot(void);       /* main(): snapshot RSR, publish+clear old crumbs */
void gw_crumb_fault(uint32_t type, uint32_t pc, uint32_t lr); /* it.c, before BSOD */
void gw_crumb_wwdg(void);       /* WWDG early-wakeup callback */
void gw_crumb_heartbeat(void);  /* common_emu_frame_loop(), throttled inside */
void gw_crumb_modal(uint32_t code, uint32_t input_bits); /* entry: state+evidence */
void gw_crumb_modal_exit(void); /* modal loop returned normally */
void gw_crumb_wdog_armed(int armed); /* wdog_enable(): DR15 = outcome
  * bit0 RCC APB3ENR WWDGEN, bit1 CR WDGA, bit2 retried -- lets the next
  * wild unarmed boot be diagnosed from the backup domain alone */
void gw_crumb_wdog_armed(int armed); /* wdog_enable(): DR15 = outcome
  * bit0 RCC APB3ENR WWDGEN, bit1 CR WDGA, bit2 retried -- lets the next
  * wild unarmed boot be diagnosed from the backup domain alone */

#endif /* GW_CRASH_CRUMBS_H */
