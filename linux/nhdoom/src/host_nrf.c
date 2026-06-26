/* nhdoom host port: backing for the dummy nRF peripherals.
 *
 *  - NVMC: held permanently "ready"; the actual word writes in w_wad.c land
 *          directly in the mmap'd internal-flash-cache region (host_main.c).
 *  - QSPI: the engine starts a column read by writing the READ.{CNT,SRC,DST}
 *          registers + TASKS_READSTART, then spins on EVENTS_READY.  A polling
 *          thread emulates the DMA: it performs the memcpy and raises
 *          EVENTS_READY, exactly mirroring the hardware handshake.  Every
 *          consumer in r_fast_stuff.c waits on EVENTS_READY before touching the
 *          destination buffer, so a synchronous copy-on-trigger is correct.
 */
#include <pthread.h>
#include <string.h>
#include "nrf.h"
#include "main.h"   /* NH_GPIO_Type */

NRF_NVMC_Type  nh_nvmc  = { .READY = 1 };   /* READY(1) != Busy(0) -> gates pass */
NRF_QSPI_Type  nh_qspi  = { 0 };
NRF_TIMER_Type nh_timer3 = { 0 };
NH_GPIO_Type   nh_gpio_p0 = { .IN = 0xFFFFFFFFu };  /* pull-ups: nothing pressed */
NH_GPIO_Type   nh_gpio_p1 = { .IN = 0xFFFFFFFFu };

static void *qspi_dma_thread(void *arg)
{
    (void)arg;
    for (;;) {
        if (nh_qspi.TASKS_READSTART) {
            /* CNT/SRC/DST already written before TASKS_READSTART (and before
             * EVENTS_READY was cleared), so they are stable here. */
            void *dst = (void *)(uintptr_t)nh_qspi.READ.DST;
            const void *src = (const void *)(uintptr_t)nh_qspi.READ.SRC;
            uint32_t cnt = nh_qspi.READ.CNT;
            if (dst && src && cnt)
                memcpy(dst, src, cnt);
            __sync_synchronize();
            nh_qspi.EVENTS_READY = 1;
            nh_qspi.TASKS_READSTART = 0;
        }
    }
    return 0;
}

void nh_start_qspi_dma(void)
{
    pthread_t t;
    pthread_create(&t, 0, qspi_dma_thread, 0);
    pthread_detach(t);
}

/* qspi.h HAL surface. External flash is the read-only mmap'd WAD; save-game /
 * settings writes (g_game.c) are no-ops on host (we are not persisting). */
uint32_t qspiFlashGetSize(void) { return 8u * 1024u * 1024u; }
void qspiFlashProgram(uint32_t address, uint8_t *data, uint32_t size)
{ (void)address; (void)data; (void)size; }
void qspiFlashErasePage4k(uint32_t address) { (void)address; }
void qspiFlashErasePage64k(uint32_t address) { (void)address; }
