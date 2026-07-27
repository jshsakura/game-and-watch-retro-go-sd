/* Harness-only stub for the user-pending rtcbat.h.  The firmware build pulls
 * in the real header (Core/Inc/porting/tamapoke/rtcbat.h, not yet written);
 * the host harness only needs the two symbols tamapoke_ui.cpp calls.
 *
 * When the real header lands at Core/Inc/porting/tamapoke/rtcbat.h this stub
 * will start a redefinition fight and should be deleted.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint8_t batPercent(void) { return 87; }
static inline bool    batCharging(void) { return false; }

#ifdef __cplusplus
}
#endif
