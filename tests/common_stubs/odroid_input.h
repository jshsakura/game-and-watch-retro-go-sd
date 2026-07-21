/* Host-test stub, mirrors retro-go-stm32/components/odroid/odroid_input.h
 * (upstream-pinned submodule, not ours to edit) closely enough that
 * ODROID_INPUT_* values and struct layout match what common.c indexes. */
#ifndef STUB_ODROID_INPUT_H
#define STUB_ODROID_INPUT_H
#include <stdint.h>

typedef enum {
    ODROID_INPUT_UP = 0,
    ODROID_INPUT_RIGHT,
    ODROID_INPUT_DOWN,
    ODROID_INPUT_LEFT,
    ODROID_INPUT_SELECT,
    ODROID_INPUT_START,
    ODROID_INPUT_A,
    ODROID_INPUT_B,
    ODROID_INPUT_MENU,
    ODROID_INPUT_VOLUME,
    ODROID_INPUT_POWER,
    ODROID_INPUT_X,
    ODROID_INPUT_Y,
    ODROID_INPUT_MAX,
    ODROID_INPUT_ANY,
} odroid_gamepad_key_t;

typedef struct {
    uint8_t values[ODROID_INPUT_MAX];
    uint16_t bitmask;
} odroid_gamepad_state_t;

typedef enum {
    ODROID_BATTERY_CHARGE_STATE_BATTERY_MISSING,
    ODROID_BATTERY_CHARGE_STATE_CHARGING,
    ODROID_BATTERY_CHARGE_STATE_DISCHARGING,
    ODROID_BATTERY_CHARGE_STATE_FULL,
} odroid_battery_charge_state_t;

typedef struct {
    int millivolts;
    int percentage;
    odroid_battery_charge_state_t state;
} odroid_battery_state_t;

odroid_battery_state_t odroid_input_read_battery(void);

#endif
