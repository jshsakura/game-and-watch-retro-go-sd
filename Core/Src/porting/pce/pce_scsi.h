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

/* DIAG (it25): per-instruction PC ring. h6280_run() calls pce_scsi_pc_tick()
 * every instruction while g_pcecd_trace is set; the ring is dumped right before
 * the boot-retry SCSI reset to expose what the game checked. Remove later. */
extern int g_pcecd_trace;
void pce_scsi_pc_tick(uint16_t pc);
