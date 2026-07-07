#ifndef STUB_BQ24072_H
#define STUB_BQ24072_H
/* Minimal stand-in for Core/Inc/bq24072.h: only the pieces rg_clock.c
 * touches (the charging-exception check in rg_clock_show). Each host test
 * file defines its own bq24072_get_state() so it can drive the states it
 * cares about. */
typedef enum {
    BQ24072_STATE_MISSING,
    BQ24072_STATE_CHARGING,
    BQ24072_STATE_DISCHARGING,
    BQ24072_STATE_FULL,
} bq24072_state_t;

bq24072_state_t bq24072_get_state(void);
#endif
