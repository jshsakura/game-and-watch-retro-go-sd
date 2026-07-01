#include <stdbool.h>

#include "softspi.h"
#include "main.h"

/* MOSI-setup pause before each SCK rising edge. DO NOT REDUCE to raise the bit clock:
 * the user already tried raising the soft-SPI clock — it corrupted SD access AND ended up
 * SLOWER (faster bit clock → the card mis-samples → CRC errors → FatFs re-reads the sector
 * → net slower + broken). 16 NOPs is at the card's reliable limit for this bit-bang wiring.
 * The SD-bandwidth win for video/MP3 is NOT here — it's in the READ PATTERN (multi-block
 * reads, fewer/larger transfers), not the per-bit clock. */
__attribute__((always_inline))
static inline void gpio_pause() {
    __asm("NOP"); __asm("NOP"); __asm("NOP"); __asm("NOP");
    __asm("NOP"); __asm("NOP"); __asm("NOP"); __asm("NOP");
    __asm("NOP"); __asm("NOP"); __asm("NOP"); __asm("NOP");
    __asm("NOP"); __asm("NOP"); __asm("NOP"); __asm("NOP");
}

static void delay_us(uint32_t usec) {
    uint32_t cycles_per_us = SystemCoreClock / 1000000;
    uint32_t nop_count = cycles_per_us * (usec / 2);

    while(nop_count--) {
        __asm("NOP");
    }
}

/* Inner bit-bang loop. Uses direct GPIO BSRR/IDR register writes instead of
 * HAL_GPIO_WritePin/ReadPin: at the SD data-phase clock (DelayUs == 0) every bit
 * was spending ~3 HAL function calls (~tens of cycles each), which capped SD
 * throughput around ~220 KB/s and made video frame reads ~31ms each. The bit
 * PROTOCOL is identical (set MOSI -> setup pause -> SCK high -> sample MISO ->
 * SCK low), only the per-bit overhead drops, so timing/correctness — and thus
 * savestate WRITE integrity, which shares this path — is preserved; the clock
 * stays far below the SD card's SPI-mode limit. CS is toggled once per transfer
 * (negligible), so it keeps the simpler HAL calls. */
static void __SoftSpi_WriteRead(SoftSPI *spi, const uint8_t *txData, uint8_t *rxData,
                                uint32_t len, bool txDummy, bool csEnable) {
    if (!len)
        return;

    GPIO_TypeDef *sck_port  = spi->sck.port;
    GPIO_TypeDef *mosi_port = spi->mosi.port;
    GPIO_TypeDef *miso_port = spi->miso.port;
    const uint32_t sck_set  = spi->sck.pin;
    const uint32_t sck_clr  = (uint32_t)spi->sck.pin << 16;
    const uint32_t mosi_set = spi->mosi.pin;
    const uint32_t mosi_clr = (uint32_t)spi->mosi.pin << 16;
    const uint32_t miso_pin = spi->miso.pin;
    const uint32_t dly      = spi->DelayUs;

    sck_port->BSRR = sck_clr;                       /* SCK idle low */
    if (csEnable)
        HAL_GPIO_WritePin(spi->cs.port, spi->cs.pin,
                          spi->csIsInverted ? GPIO_PIN_SET : GPIO_PIN_RESET);
    else if (spi->cs.port)
        HAL_GPIO_WritePin(spi->cs.port, spi->cs.pin,
                          spi->csIsInverted ? GPIO_PIN_RESET : GPIO_PIN_SET);

    for (uint32_t i = 0; i < len; i++) {
        uint8_t txByte = txDummy ? txData[0] : txData[i];
        uint8_t rxByte = 0;

        for (int j = 7; j >= 0; j--) {
            mosi_port->BSRR = (txByte & (1u << j)) ? mosi_set : mosi_clr;
            gpio_pause();                            /* MOSI setup before clock edge */
            sck_port->BSRR = sck_set;                /* SCK high — card samples MOSI */
            if (dly) delay_us(dly);

            rxByte = (uint8_t)((rxByte << 1) |
                               ((miso_port->IDR & miso_pin) ? 1u : 0u));

            sck_port->BSRR = sck_clr;                /* SCK low */
            if (dly) delay_us(dly);
        }

        if (rxData)
            rxData[i] = rxByte;
    }

     if (csEnable)
        HAL_GPIO_WritePin(spi->cs.port, spi->cs.pin,
                          spi->csIsInverted ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

void SoftSpi_WriteRead(SoftSPI *spi, const uint8_t *txData, uint8_t *rxData, uint32_t len) {
    __SoftSpi_WriteRead(spi, txData, rxData, len, false, !!spi->cs.port);
}

void SoftSpi_WriteDummyRead(SoftSPI *spi, uint8_t *rxData, uint32_t len) {
    uint8_t dummy = 0xFF;
    __SoftSpi_WriteRead(spi, &dummy, rxData, len, true, !!spi->cs.port);
}

void SoftSpi_WriteDummyReadCsLow(SoftSPI *spi, uint8_t *rxData, uint32_t len) {
    uint8_t dummy = 0xFF;
    __SoftSpi_WriteRead(spi, &dummy, rxData, len, true, false);
}
