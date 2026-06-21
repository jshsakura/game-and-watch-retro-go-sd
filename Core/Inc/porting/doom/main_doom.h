#ifndef _MAIN_DOOM_H_
#define _MAIN_DOOM_H_

#include <stdint.h>

/* Homebrew overlay entry point. Mirrors the other homebrew apps
 * (app_main_music / app_main_zelda3): load_state requests "continue"
 * (resume from the savestate slot), start_paused boots into the pause
 * menu, save_slot selects the savestate slot. */
int app_main_doom(uint8_t load_state, uint8_t start_paused, int8_t save_slot);

#endif /* _MAIN_DOOM_H_ */
