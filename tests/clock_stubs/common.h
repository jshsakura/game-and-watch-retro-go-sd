#ifndef STUB_COMMON_H
#define STUB_COMMON_H
#include <stdint.h>
#include "odroid_system.h"
extern const uint8_t volume_tbl[ODROID_AUDIO_VOLUME_MAX + 1];
/* mirrors Core/Inc/porting/common.h: a row set to this enabled value is fully
 * omitted from odroid_overlay_dialog (zero height, not drawn, not a
 * navigation stop) instead of just greyed like -1. */
#define ODROID_DIALOG_HIDDEN (-2)
#endif
