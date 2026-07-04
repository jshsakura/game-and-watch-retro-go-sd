#pragma once
#include <string.h>
enum { ODROID_INPUT_UP=0, ODROID_INPUT_DOWN, ODROID_INPUT_LEFT, ODROID_INPUT_RIGHT,
       ODROID_INPUT_A, ODROID_INPUT_B, ODROID_INPUT_START, ODROID_INPUT_SELECT,
       ODROID_INPUT_MENU, ODROID_INPUT_VOLUME, ODROID_INPUT_MAX };
typedef struct { int values[ODROID_INPUT_MAX]; } odroid_gamepad_state;
static inline void odroid_input_gamepad_read(odroid_gamepad_state* s){ memset(s,0,sizeof(*s)); }
