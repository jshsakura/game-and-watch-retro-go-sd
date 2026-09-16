/* Host stubs for the Sega CD RAM probe (tests/segacd_stubs).
 *
 * The real Core/Src/segacd_ram_probe.c is compiled WHOLE into the host test
 * (tests/test_segacd_ram_probe.c #includes it with -DSEGACD_RAM_PROBE). On
 * the device its includes pull the firmware HAL/LCD stack; here every quoted
 * include the probe names resolves into this directory first, so the probe's
 * own logic — the gate-4 claim sequence, the 14-region R/W pattern sweep and
 * the report path — runs against plain host memory.
 *
 * Signatures mirror the firmware headers exactly (odroid_overlay.h:55,
 * main.h:131, gw_lcd.h) so the probe compiles under -Wall -Wextra with the
 * same call shapes it uses on hardware. The LCD recorder below keeps every
 * drawn line so the test can assert on the report the DEVICE would show.
 */
#ifndef SEGACD_STUBS_MAIN_H
#define SEGACD_STUBS_MAIN_H
#include <stdint.h>
void wdog_refresh(void);
#endif
