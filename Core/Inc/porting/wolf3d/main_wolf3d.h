#ifndef _MAIN_WOLF3D_H_
#define _MAIN_WOLF3D_H_

#include <stdint.h>

/* Homebrew overlay entry point (mirrors app_main_doom / app_main_zelda3). */
int app_main_wolf3d(uint8_t load_state, uint8_t start_paused, int8_t save_slot);

#endif /* _MAIN_WOLF3D_H_ */
