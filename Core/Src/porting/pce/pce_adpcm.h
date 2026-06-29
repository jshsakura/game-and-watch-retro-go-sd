/* PC Engine CD ADPCM (OKI MSM5205). $1808-$180E register block, 64KB RAM,
 * decode + 22050Hz resample for the PCE audio mixer. See pce_adpcm.c. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

void    pce_adpcm_reset(void);
void    pce_adpcm_write(uint8_t reg, uint8_t val);   /* reg = A & 0x0F */
uint8_t pce_adpcm_read(uint8_t reg);
void    pce_adpcm_dma_byte(uint8_t val);             /* SCSI->ADPCM DMA: one byte to RAM */
bool    pce_adpcm_playing(void);
int     pce_adpcm_fill(int16_t *out, int frames);    /* stereo @ 22050; 0 if idle */
