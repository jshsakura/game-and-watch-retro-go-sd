/* nhdoom DEVICE port: backing storage for the dummy nRF peripherals.
 *
 *  - NVMC : held permanently "ready"; w_wad.c word-writes target the internal-
 *           flash cache region, which on device is the (read-only) XIP WAD, so
 *           the save/program path is a no-op (we do not persist to it here).
 *  - QSPI : the register block whose lazy-flush accessor (nh_qspi_service in
 *           nrf.h) performs the XIP->RAM memcpy. No DMA hardware needed because
 *           the source is memory-mapped OCTOSPI flash.
 *  - GPIO : onboard A/B button block in i_video.c reads these as "not pressed"
 *           (pull-ups, all ones); real input arrives through getKeys().
 *
 * GPL-2.0 (see external/nh-doom, next-hack/nRF52840Doom lineage).
 */
#include "nrf.h"
#include "main.h"   /* NH_GPIO_Type */
#include "qspi.h"

NRF_NVMC_Type  nh_nvmc   = { .READY = 1 };          /* READY(1) != Busy(0) */
NRF_QSPI_Type  nh_qspi   = { 0 };
NRF_TIMER_Type nh_timer3 = { 0 };
NH_GPIO_Type   nh_gpio_p0 = { .IN = 0xFFFFFFFFu };  /* nothing pressed */
NH_GPIO_Type   nh_gpio_p1 = { .IN = 0xFFFFFFFFu };

/* qspi.h HAL surface. External flash is the read-only XIP WAD; programming and
 * erasing are no-ops on device (no persistence path from the engine). */
uint32_t qspiFlashGetSize(void) { return 8u * 1024u * 1024u; }
void qspiFlashProgram(uint32_t address, uint8_t *data, uint32_t size)
{ (void)address; (void)data; (void)size; }
void qspiFlashErasePage4k(uint32_t address)  { (void)address; }
void qspiFlashErasePage64k(uint32_t address) { (void)address; }
