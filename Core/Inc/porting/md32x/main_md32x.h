#ifndef _MAIN_MD32X_H_
#define _MAIN_MD32X_H_

#include <stdint.h>

/* Launcher entry point for the Sega 32X (picodrive) core. Kept un-namespaced
 * (NOT renamed by md32x_redefines) so rg_emulators can dispatch to it. */
void app_main_md32x(uint8_t load_state, uint8_t start_paused, int8_t save_slot);

#endif /* _MAIN_MD32X_H_ */
