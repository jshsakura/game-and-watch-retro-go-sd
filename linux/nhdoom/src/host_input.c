/* nhdoom host port: replaces src/keyboard.c. Headless => no keys pressed. */
#include "keyboard.h"

void initKeyboard(void) {}

void getKeys(key_t *keys)
{
    if (keys) *keys = 0;
}
