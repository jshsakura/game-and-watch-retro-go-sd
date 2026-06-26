#ifndef _MAIN_NHDOOM_H_
#define _MAIN_NHDOOM_H_

#include <stdint.h>

/* Homebrew overlay entry point for the next-hack DOOM engine (Phase 2).
 * Mirrors app_main_doom (the doomgeneric checkpoint) so the rg_emulators.c
 * dispatch can call either depending on the USE_NHDOOM build gate. */
int app_main_nhdoom(uint8_t load_state, uint8_t start_paused, int8_t save_slot);

#endif /* _MAIN_NHDOOM_H_ */
