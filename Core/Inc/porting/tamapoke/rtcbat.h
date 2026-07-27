/* Clock and battery, under upstream's header name.
 *
 * Upstream talks to a PCF85063 RTC and an AXP2101 PMU over I2C. Both have
 * local equivalents -- the STM32 RTC and the BQ24072 charger -- so the port
 * keeps the names the UI already calls and swaps what is behind them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tamapoke_shim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 0-100, filtered. The raw reading swings with load, and the pet's status bar
 * sits on screen permanently, so an unfiltered value visibly jitters. */
int tamapoke_bat_percent(void);
bool tamapoke_bat_charging(void);

/* Seconds since 1970 and the wall clock split out, for the scene's day/night
 * tinting and the offline-aging maths. */
uint32_t tamapoke_epoch(void);
void tamapoke_local_time(int *hour, int *minute);

/* Whether the user has ever set the clock. The pet ages by diffing epochs
 * across power-off, so an unset RTC must freeze aging rather than hand it the
 * full two-week catch-up on first run. */
bool tamapoke_clock_is_set(void);

#ifdef __cplusplus
}

/* Upstream's spellings, so the UI code is unmodified. */
static inline int batPercent() { return tamapoke_bat_percent(); }
static inline bool batCharging() { return tamapoke_bat_charging(); }

#endif
