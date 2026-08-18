/* nhdoom DEVICE port: replaces src/keyboard.c.
 * Translates the Game & Watch button bitfield (gw_buttons.h, buttons_get())
 * into the next-hack I2C_GAMEPAD key bitfield the engine consumes in getKeys().
 * Mapping mirrors the doomgeneric port's DOOM_KEYMAP intent.
 *
 * GPL-2.0 (see external/nh-doom, next-hack/nRF52840Doom lineage).
 */
#include "keyboard.h"
#include "gw_buttons.h"

void initKeyboard(void) {}

void getKeys(key_t *keys)
{
    if (!keys) return;
    uint32_t b = buttons_get();
    key_t k = 0;
    if (b & B_Up)     k |= KEY_UP;
    if (b & B_Down)   k |= KEY_DOWN;
    if (b & B_Left)   k |= KEY_LEFT;
    if (b & B_Right)  k |= KEY_RIGHT;
    if (b & B_A)      k |= KEY_FIRE;
    if (b & B_B)      k |= KEY_USE;
    if (b & B_GAME)   k |= KEY_ALT;    /* strafe/run modifier */
    if (b & B_TIME)   k |= KEY_MAP;    /* automap */
    if (b & B_START)  k |= KEY_MENU;
    if (b & B_SELECT) k |= KEY_CHGW;   /* change weapon */
    *keys = k;
}
