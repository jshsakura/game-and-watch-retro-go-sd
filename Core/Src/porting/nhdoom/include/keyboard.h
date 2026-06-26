/* nhdoom host port: shadow of src/keyboard.h (I2C_GAMEPAD key map). */
#ifndef KEYBOARD_H
#define KEYBOARD_H
#include <stdint.h>
#include "main.h"

/* I2C gamepad bit assignments (verbatim from upstream keyboard.h). */
#define KEY_MAP   0x0001
#define KEY_ALT   0x0002
#define KEY_USE   0x0004
#define KEY_LEFT  0x0008
#define KEY_DOWN  0x0010
#define KEY_FIRE  0x0020
#define KEY_CHGW  0x0040
#define KEY_UP    0x0080
#define KEY_MENU  0x0100
#define KEY_RIGHT 0x0200

void initKeyboard(void);
void getKeys(key_t *keys);

/* non-radio build: wireless audio hooks are empty. */
#define disableWirelessAudio()
#define restoreWirelessAudio()
#define initWirelessAudio()
#endif
