#pragma once

/* What the Sega CD core is configured with, in one place for the firmware
 * (main_segacd.c) and the host rig (tools/pico_host_segacd). The rig once
 * enabled POPT_EN_MCD_GFX while the device did not, so every game that draws
 * through the rotation/scaling ASIC rendered on the host and not on the
 * console, and nothing noticed. Both sides now read these. */

#include "pico/pico.h"

/* Mono output: the unit has one speaker. MCD_GFX is the Word-RAM ASIC; without
 * it PicoDrive still raises the completion interrupt but never draws. */
#define SEGACD_PICO_OPT (POPT_EN_FM | POPT_EN_PSG | POPT_EN_Z80 |          \
                         POPT_EN_MCD_PCM | POPT_EN_MCD_CDDA |              \
                         POPT_EN_MCD_GFX | POPT_ACC_SPRITES)

/* PicoDrawUpdateHighPal() pins HighPal[0xe0] to black for OSD use. When a game
 * switches H40->H32 or 240->224 lines, PicoDrive stops drawing the margins and
 * the previous mode's pixels stay there as colored bars; the frontend owns
 * clearing them, with this index. */
#define SEGACD_BORDER_INDEX 0xe0
