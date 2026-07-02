/* PC Engine CD-ROM2 ($1800-$180F) SCSI target — Phase 2.2, iteration 1.
 *
 * The System Card BIOS drives this register block as a SCSI-1 initiator to read
 * the data track off the disc. This module is the SCSI *target* state machine:
 * it accepts a 6-byte CDB, runs the COMMAND -> DATA-IN -> STATUS -> MESSAGE
 * phases, and feeds sector data from the CUE/BIN layer (pce_cd). Audio (ADPCM /
 * CD-DA) is stubbed for now — boot/data first. Expect on-device iteration. */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "pce_cd.h"

/* Attach (or detach) the mounted disc. Called at .cue launch. */
void pce_scsi_set_disc(const pce_cd_toc_t *toc, bool present);

/* Reset the SCSI/CD state (called from ResetPCE). */
void pce_scsi_reset(void);

/* CD-ROM2 register block at $1800-$180F. `reg` is the low nibble (A & 0x0F). */
uint8_t pce_scsi_read(uint8_t reg);
void    pce_scsi_write(uint8_t reg, uint8_t val);

/* Per-frame poll: advances pending reads and asserts IRQ when data/status is
 * ready (kept out of the hot CPU read path). Returns true if an IRQ is pending. */
void pce_scsi_run(void);

/* Fill `frames` stereo int16 samples of CD-DA (Red Book audio / BGM) at 22050Hz
 * (half the CD rate). Returns frames produced, 0 if no audio is playing. */
int pce_scsi_cdda_fill(int16_t *out, int frames);

/* CD-DA playback snapshot for savestates: [0]=play [1]=lba [2]=start [3]=end
 * [4]=mode. LoadState resets the SCSI to idle (correct for data transfers), which
 * used to also kill the BGM — the game believes its music is still playing, so a
 * resume stayed silent until the next track change. Save/restore these 5 words to
 * re-arm the stream. */
#define PCE_SCSI_CDDA_STATE_WORDS 5
void pce_scsi_cdda_get(uint32_t out[PCE_SCSI_CDDA_STATE_WORDS]);
void pce_scsi_cdda_set(const uint32_t in[PCE_SCSI_CDDA_STATE_WORDS]);
