#ifndef STUB_ODROID_INPUT_H
#define STUB_ODROID_INPUT_H
#include <stdint.h>
enum { ODROID_INPUT_UP, ODROID_INPUT_DOWN, ODROID_INPUT_LEFT, ODROID_INPUT_RIGHT,
       ODROID_INPUT_A, ODROID_INPUT_B, ODROID_INPUT_SELECT, ODROID_INPUT_START,
       ODROID_INPUT_VOLUME, ODROID_INPUT_POWER, ODROID_INPUT_MAX };
typedef struct { uint8_t values[ODROID_INPUT_MAX]; uint32_t bitmask; } odroid_gamepad_state_t;
void odroid_input_read_gamepad(odroid_gamepad_state_t *s);
int odroid_input_read_battery(void);
#endif
