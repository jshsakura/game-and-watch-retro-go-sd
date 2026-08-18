/* nhdoom DEVICE port: replacement for the nRF52840 CMSIS header.
 *
 * Provides the registers + intrinsics the COMPILED engine touches:
 *   - NRF_NVMC  : internal-flash controller (w_wad.c flash cache) -> held ready
 *   - NRF_QSPI  : external-flash DMA reader  (r_fast_stuff.c)      -> XIP shim
 *   - NRF_TIMER3: profiling timer (only i_main.c, which we exclude)
 *
 * On the Game & Watch the WAD lives in memory-mapped OCTOSPI XIP flash
 * (0x90000000), so the engine's "QSPI DMA" is just a memcpy from a directly
 * readable source. host_nrf.c emulated the DMA with a polling thread; on the
 * single-core MCU there is no thread, so instead NRF_QSPI is a function-call
 * macro: every register access flushes any pending transfer first. Because the
 * engine writes CNT/SRC/DST, then TASKS_READSTART, then reads EVENTS_READY (in
 * the same straight-line code), the FIRST access after TASKS_READSTART=1
 * performs the copy and raises EVENTS_READY -- so both the immediate
 *   while (NRF_QSPI->EVENTS_READY == 0);
 * and the deferred (start ... work ... wait) patterns observe completion, with
 * zero engine edits. Mirrors host_nrf.c semantics exactly.
 */
#ifndef NH_NRF_H
#define NH_NRF_H
#include <stdint.h>
#include <string.h>

/* ---- core intrinsics ----
 * Defined ONLY when the CMSIS core header is NOT in the translation unit (i.e.
 * pure engine files such as w_wad.c, the lone __DMB() user). Device HAL files
 * that include the STM32 HAL pull CMSIS first (__CMSIS_GCC_H), which provides
 * these as __STATIC_FORCEINLINE functions; defining our own there would be a
 * redefinition. Hence the guard. */
#ifndef __CMSIS_GCC_H
static inline void __DMB(void) { __asm volatile ("dmb 0xF":::"memory"); }
static inline void __DSB(void) { __asm volatile ("dsb 0xF":::"memory"); }
static inline void __ISB(void) { __asm volatile ("isb 0xF":::"memory"); }
static inline void __NOP(void) { __asm volatile ("nop"); }
static inline void NVIC_SystemReset(void) { for (;;) {} }
#endif

/* ---- NVMC (internal flash) : never busy, writes are no-ops on device ---- */
typedef struct {
    volatile uint32_t READY;
    volatile uint32_t CONFIG;
    volatile uint32_t ERASEPAGE;
    volatile uint32_t ERASEALL;
    volatile uint32_t ICACHECNF;
} NRF_NVMC_Type;
extern NRF_NVMC_Type nh_nvmc;
#define NRF_NVMC (&nh_nvmc)
#define NVMC_READY_READY_Busy   0u
#define NVMC_CONFIG_WEN_Pos     0u
#define NVMC_CONFIG_WEN_Ren     0u
#define NVMC_CONFIG_WEN_Wen     1u
#define NVMC_CONFIG_WEN_Een     2u

/* ---- QSPI (external-flash DMA) : synchronous XIP-memcpy shim ---- */
typedef struct {
    volatile uint32_t CNT;
    volatile uint32_t SRC;
    volatile uint32_t DST;
} nh_qspi_read_t;
typedef struct {
    volatile uint32_t TASKS_READSTART;
    volatile uint32_t EVENTS_READY;
    nh_qspi_read_t READ;
} NRF_QSPI_Type;
extern NRF_QSPI_Type nh_qspi;

/* Flush a pending transfer, then hand back the register block. Called on EVERY
 * NRF_QSPI access via the macro below, so completion is observed lazily but
 * always before the engine reads the destination buffer. */
static inline NRF_QSPI_Type *nh_qspi_service(void)
{
    if (nh_qspi.TASKS_READSTART) {
        nh_qspi.TASKS_READSTART = 0;
        if (nh_qspi.READ.DST && nh_qspi.READ.SRC && nh_qspi.READ.CNT)
            memcpy((void *)(uintptr_t)nh_qspi.READ.DST,
                   (const void *)(uintptr_t)nh_qspi.READ.SRC,
                   nh_qspi.READ.CNT);
        nh_qspi.EVENTS_READY = 1;
    }
    return &nh_qspi;
}
#define NRF_QSPI (nh_qspi_service())

/* ---- TIMER3 (unused by compiled set, defined for completeness) ---- */
typedef struct { volatile uint32_t TASKS_CAPTURE[6]; volatile uint32_t CC[6]; } NRF_TIMER_Type;
extern NRF_TIMER_Type nh_timer3;
#define NRF_TIMER3 (&nh_timer3)

#endif
