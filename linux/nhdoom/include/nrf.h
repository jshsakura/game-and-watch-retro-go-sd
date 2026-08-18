/* nhdoom host port: dummy replacement for the nRF52840 CMSIS header.
 * Provides just the registers + intrinsics the COMPILED engine files touch:
 *   - NRF_NVMC : internal-flash controller (w_wad.c flash cache)  -> no-op gate
 *   - NRF_QSPI : external-flash DMA reader  (r_fast_stuff.c)       -> emulated
 *   - NRF_TIMER3 : profiling timer (only i_main.c, which we exclude)
 * The actual data movement is backed by host memory (see host_nrf.c). */
#ifndef NH_NRF_H
#define NH_NRF_H
#include <stdint.h>

/* ---- core intrinsics (all no-ops on host) ---- */
static inline void __DMB(void) {}
static inline void __DSB(void) {}
static inline void __ISB(void) {}
static inline void __NOP(void) {}
static inline void __enable_irq(void) {}
static inline void __disable_irq(void) {}
static inline void NVIC_SystemReset(void) { }

/* ---- NVMC (internal flash) ---- */
typedef struct {
    volatile uint32_t READY;
    volatile uint32_t CONFIG;
    volatile uint32_t ERASEPAGE;
    volatile uint32_t ERASEALL;
    volatile uint32_t ICACHECNF;
} NRF_NVMC_Type;
extern NRF_NVMC_Type nh_nvmc;
#define NRF_NVMC (&nh_nvmc)
/* READY field is held at READY(=1); Busy(=0) so every `while(READY==Busy)`
 * gate exits immediately. WEN values are irrelevant (writes go to mmap). */
#define NVMC_READY_READY_Busy   0u
#define NVMC_CONFIG_WEN_Pos     0u
#define NVMC_CONFIG_WEN_Ren     0u
#define NVMC_CONFIG_WEN_Wen     1u
#define NVMC_CONFIG_WEN_Een     2u

/* ---- QSPI (external flash DMA) ---- */
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
#define NRF_QSPI (&nh_qspi)

/* ---- TIMER3 (unused by compiled set, defined for safety) ---- */
typedef struct { volatile uint32_t TASKS_CAPTURE[6]; volatile uint32_t CC[6]; } NRF_TIMER_Type;
extern NRF_TIMER_Type nh_timer3;
#define NRF_TIMER3 (&nh_timer3)

#endif
