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

/* CD save/load + audio-mix API. main_pce.c (upstream) calls these unconditionally,
 * but every CD-state call sits behind an `ext == "cue"` guard and there are no
 * .cue games without an SD card to stream them from — so on a flash-only build
 * these only need to LINK, never to run. The audio-fill pair is the exception:
 * it can be called from the mixer, and returning 0 frames == "no CD audio",
 * which is exactly right. */
#include "pce_cd.h"
#include "pce_scsi.h"
#include "pce_adpcm.h"

bool pce_cd_parse_cue(const char *cue_path, pce_cd_toc_t *toc) { (void)cue_path; (void)toc; return false; }
void pce_cd_close(void) { }

void pce_scsi_run(void) { }
void pce_scsi_reset(void) { }
void pce_scsi_set_disc(const pce_cd_toc_t *toc, bool present) { (void)toc; (void)present; }
void pce_scsi_post_restore(void) { }
bool pce_scsi_cdda_prefetch(void) { return false; }
int  pce_scsi_cdda_fill(int16_t *out, int frames) { (void)out; (void)frames; return 0; }
uint32_t pce_scsi_adpcm_volume(void) { return 0; }
void pce_scsi_state_get(pce_scsi_state_t *st) { (void)st; }
void pce_scsi_state_set(const pce_scsi_state_t *st) { (void)st; }
void pce_scsi_cdda_get(uint32_t out[PCE_SCSI_CDDA_STATE_WORDS]) { (void)out; }
void pce_scsi_cdda_set(const uint32_t in[PCE_SCSI_CDDA_STATE_WORDS]) { (void)in; }

int  pce_adpcm_fill(int16_t *out, int frames) { (void)out; (void)frames; return 0; }
void pce_adpcm_frame_end(void) { }           /* called every frame, CD unit or not */
void pce_adpcm_reset(void) { }
void pce_adpcm_reconcile_load(void) { }
uint8_t *pce_adpcm_ram(void) { return 0; }  /* only reached inside the cue guard */
void pce_adpcm_get(uint32_t out[PCE_ADPCM_STATE_WORDS]) { (void)out; }
void pce_adpcm_set(const uint32_t in[PCE_ADPCM_STATE_WORDS]) { (void)in; }

#endif /* !SD_CARD */
