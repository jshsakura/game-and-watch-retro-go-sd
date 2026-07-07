/* PCE CD-ROM2 stubs for SD_CARD=0 (flash-only) builds.
 *
 * The pce-go core references the SCSI target unconditionally from its
 * $1800-$180F IO decode and the h6280 trace hook, but the real
 * pce_scsi.c/pce_adpcm.c/pce_cd.c are only built for SD builds — CD images
 * can't fit (or stream from) flash-only systems, and upstream guidance is to
 * keep the CD stack out of them to preserve flash headroom for HuCards.
 * These stubs behave like a console with no CD unit attached.
 */

#include <stdint.h>

#if !SD_CARD

int g_pcecd_trace = 0;                       /* keeps the h6280 hook a no-op */

void pce_scsi_pc_tick(uint16_t pc) { (void)pc; }

uint8_t pce_scsi_read(uint8_t reg)
{
    (void)reg;
    return 0xFF;                             /* open bus: no CD unit present */
}

void pce_scsi_write(uint8_t reg, uint8_t val)
{
    (void)reg;
    (void)val;
}

#endif /* !SD_CARD */
