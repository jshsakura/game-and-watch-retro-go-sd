/* Crash breadcrumbs -- see gw_crash_crumbs.h for the register map and the
 * reasoning. Writes go through HAL_RTCEx_BKUPWrite on the global hrtc
 * (same pattern as gw_boot_rescue.c). The heartbeat is throttled to every
 * 32 emu frames so the backup-domain write cost stays ~1 Hz. */

#include <string.h>
#include "main.h"
#include "gw_crash_crumbs.h"

extern RTC_HandleTypeDef hrtc;
extern uint32_t g_common_emu_frames;
extern uint32_t g_common_drawn_frames;

#define CRUMB_MAGIC       0xC4AB0001u
#define CRUMB_CAUSE_FAULT 1u
#define CRUMB_CAUSE_WWDG  2u
#define CRUMB_MODAL_NONE  0u

gw_crumb_t g_last_crumb;

static uint32_t crumb_rd(uint32_t dr)
{
    return HAL_RTCEx_BKUPRead(&hrtc, dr);
}

static void crumb_wr(uint32_t dr, uint32_t v)
{
    HAL_RTCEx_BKUPWrite(&hrtc, dr, v);
}

/* --- boot: publish what the previous boot died of, then re-arm --- */
void gw_crumb_boot(void)
{
    /* Reset-cause snapshot of the boot that just ended. Read-clear:
     * RSR keeps its flags until software clears them, so we grab the
     * previous boot's cause before anything else resets. */
    uint32_t rsr = READ_REG(RCC->RSR);
    __HAL_RCC_CLEAR_RESET_FLAGS();

    g_last_crumb.magic   = crumb_rd(RTC_BKP_DR2);
    g_last_crumb.cause   = crumb_rd(RTC_BKP_DR3);
    g_last_crumb.pc      = crumb_rd(RTC_BKP_DR4);
    g_last_crumb.lr      = crumb_rd(RTC_BKP_DR5);
    g_last_crumb.cfsr    = crumb_rd(RTC_BKP_DR6);
    g_last_crumb.hfsr    = crumb_rd(RTC_BKP_DR7);
    g_last_crumb.emu     = crumb_rd(RTC_BKP_DR8);
    g_last_crumb.drawn   = crumb_rd(RTC_BKP_DR9);
    g_last_crumb.rsr_prev = rsr;
    g_last_crumb.modal      = crumb_rd(RTC_BKP_DR12);
    g_last_crumb.modal_input = crumb_rd(RTC_BKP_DR13);
    g_last_crumb.modal_tr    = crumb_rd(RTC_BKP_DR14);
    g_last_crumb.wdog_armed = crumb_rd(RTC_BKP_DR15);
    g_last_crumb.bfar  = crumb_rd(RTC_BKP_DR16);
    g_last_crumb.mmfar = crumb_rd(RTC_BKP_DR17);

    /* Re-arm for this boot. Keep the heartbeat counters at zero so a
     * stale count can never masquerade as a fresh crash. */
    crumb_wr(RTC_BKP_DR2, CRUMB_MAGIC);
    crumb_wr(RTC_BKP_DR3, 0);
    crumb_wr(RTC_BKP_DR4, 0);
    crumb_wr(RTC_BKP_DR5, 0);
    crumb_wr(RTC_BKP_DR6, 0);
    crumb_wr(RTC_BKP_DR7, 0);
    crumb_wr(RTC_BKP_DR16, 0);
    crumb_wr(RTC_BKP_DR17, 0);
    crumb_wr(RTC_BKP_DR8, 0);
    crumb_wr(RTC_BKP_DR9, 0);
    crumb_wr(RTC_BKP_DR10, rsr); /* previous boot's reset cause, stays
                                     readable over SWD until the next boot */
    crumb_wr(RTC_BKP_DR11, 0);
    crumb_wr(RTC_BKP_DR12, 0);
    crumb_wr(RTC_BKP_DR13, 0);
    crumb_wr(RTC_BKP_DR14, 0);
    crumb_wr(RTC_BKP_DR15, 0);
}

/* --- modal state: the 2026-08-26 07:16 "crash" was a modal nobody
 * dismissed (emu frozen, drawn still running at LCD rate for 57 min,
 * zero fault registers). DR12 is live state; DR13/14 latch evidence. --- */
void gw_crumb_modal(uint32_t code, uint32_t input_bits)
{
    crumb_wr(RTC_BKP_DR12, code);
    crumb_wr(RTC_BKP_DR13, input_bits);
    crumb_wr(RTC_BKP_DR14, READ_REG(RTC->TR)); /* BCD HH:MM:SS at entry */
}

void gw_crumb_modal_exit(void)
{
    crumb_wr(RTC_BKP_DR12, CRUMB_MODAL_NONE); /* DR13/14 keep the evidence */
}

/* --- fault path: called from common_fault_handler_c before BSOD --- */
void gw_crumb_fault(uint32_t type, uint32_t pc, uint32_t lr)
{
    crumb_wr(RTC_BKP_DR3, CRUMB_CAUSE_FAULT);
    crumb_wr(RTC_BKP_DR4, pc);
    crumb_wr(RTC_BKP_DR5, lr);
    crumb_wr(RTC_BKP_DR6, *(volatile uint32_t *)0xE000ED28); /* CFSR */
    crumb_wr(RTC_BKP_DR7, *(volatile uint32_t *)0xE000ED2C); /* HFSR */
    uint32_t cfsr = *(volatile uint32_t *)0xE000ED28;
    /* The hardware hands us the faulting address when CFSR says it is
     * valid. Dropping it unread (base4, 2026-08-26: CFSR=0x8200,
     * BFARVALID set, address lost forever) left the 6-boot crash
     * loop undiagnosable. An invalid latch is NOT address 0 -- encode
     * it as the unmappable 0xFFFFFFFF so a quiet zero can never
     * masquerade as evidence. */
    crumb_wr(RTC_BKP_DR16, (cfsr & (1u << 15))
             ? *(volatile uint32_t *)0xE000ED38 : 0xFFFFFFFFu); /* BFAR */
    crumb_wr(RTC_BKP_DR17, (cfsr & (1u << 7))
             ? *(volatile uint32_t *)0xE000ED34 : 0xFFFFFFFFu); /* MMFAR */
    /* DR8/DR9 already hold the last heartbeat. */
    (void)type;
}

/* --- WWDG early wakeup: the watchdog is about to reset us --- */
void gw_crumb_wwdg(void)
{
    crumb_wr(RTC_BKP_DR3, CRUMB_CAUSE_WWDG);
    /* Callback context: no PC/LR of the runaway thread is reachable.
     * The heartbeat counters in DR8/DR9 are the witness. */
}

/* --- watchdog arming outcome: wdog_enable() reports, boot() re-arms --- */
void gw_crumb_wdog_armed(int armed)
{
    uint32_t v = 0;
    if (armed) v |= 2u;
    if ((RCC->APB3ENR & RCC_APB3ENR_WWDGEN) != 0) v |= 1u;
    /* bit3: the IWDG last resort actually started. KR is write-only, so
     * HAL_OK is not proof (the WWDG lesson); PR is: reset default 0, the
     * prescaler-256 init writes 6. */
    if (IWDG1->PR != 0u) v |= 8u;
    /* The retry bit is folded in by the caller's second attempt only;
     * a plain success writes 3 (clock on + armed), which is the value
     * every healthy boot leaves behind. */
    crumb_wr(RTC_BKP_DR15, v);
}

/* --- heartbeat: cheap enough to call every frame --- */
void gw_crumb_heartbeat(void)
{
    static uint32_t seq = 0;
    if ((++seq & 31u) != 0)
        return;
    crumb_wr(RTC_BKP_DR8, g_common_emu_frames);
    crumb_wr(RTC_BKP_DR9, g_common_drawn_frames);
    crumb_wr(RTC_BKP_DR11, seq);
}
